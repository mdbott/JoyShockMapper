#include "DigitalButton.h"
#include "JSMVariable.hpp"
#include "InputHelpers.h"
#include "SettingsManager.h"

void DigitalButton::Context::updateChordStack(bool isPressed, ButtonID id)
{
	if (id < ButtonID::SIZE || id >= ButtonID::T1) // Can't chord touch stick _buttons
	{
		if (isPressed)
		{
			auto foundChord = find(chordStack.begin(), chordStack.end(), id);
			if (foundChord == chordStack.end())
			{
				// COUT << "Button " << index << " is pressed!\n";
				chordStack.push_front(id); // Always push at the fromt to make it a stack
			}
		}
		else
		{
			auto foundChord = find(chordStack.begin(), chordStack.end(), id);
			if (foundChord != chordStack.end())
			{
				// COUT << "Button " << index << " is released!\n";
				chordStack.erase(foundChord); // The chord is released
			}
		}
	}
}

struct Sync
{
	pocket_fsm::StateIF *nextState = nullptr;
	chrono::steady_clock::time_point pressTime;
	const Mapping *activeMapping = nullptr;
	string nameToRelease;
	float turboTime;
	float holdTime;
	float dblPressWindow;
};

// Hidden implementation of the digital button
// This class holds all the logic related to a single digital button. It does not hold the mapping but only a reference
// to it. It also contains its various states, flags and data. The concrete state of the state machine hands off the
// instance to the next state, and so is persistent across states
struct DigitalButtonImpl : public pocket_fsm::PimplBase, public EventActionIf
{
private:
	static bool isSameKey(KeyCode key, const pair<ButtonID, KeyCode> &pair)
	{
		return pair.second == key;
	};

public:
	multimap<BtnEvent, Callback> _instantReleaseQueue;
	unsigned int _turboApplies = 0;
	unsigned int _turboReleases = 0;
	DigitalButtonImpl(JSMButton &mapping, shared_ptr<DigitalButton::Context> context)
	  : _id(mapping._id)
	  , _context(context)
	  , _press_times()
	  , _keyToRelease()
	  , _mapping(mapping)
	  , _instantReleaseQueue()
	{
	}

	const ButtonID _id; // Always ID first for easy debugging
	string _nameToRelease;
	shared_ptr<DigitalButton::Context> _context;
	chrono::steady_clock::time_point _press_times;
	optional<Mapping> _keyToRelease; // At key press, remember what to release
	const JSMButton &_mapping;
	DigitalButton *_masterPress = nullptr; // Who is this button's master in either sim or diag presses

	// Pretty wrapper
	inline float GetPressDurationMS(chrono::steady_clock::time_point time_now)
	{
		return static_cast<float>(chrono::duration_cast<chrono::milliseconds>(time_now - _press_times).count());
	}

	bool HasActiveToggle(shared_ptr<DigitalButton::Context> _context, const KeyCode &key) const
	{
		auto foundToggle = find_if(_context->activeTogglesQueue.cbegin(), _context->activeTogglesQueue.cend(),
		  [key](auto &pair)
		  {
			  return pair.second == key;
		  });
		return foundToggle != _context->activeTogglesQueue.cend();
	}

	void ClearKey()
	{
		_keyToRelease = nullopt;
		_instantReleaseQueue.clear();
		_nameToRelease.clear();
		_turboApplies = 0;
		_turboReleases = 0;
	}

	bool ReleaseInstant(BtnEvent instantEvent)
	{
		auto range = _instantReleaseQueue.equal_range(instantEvent);
		for (auto i = range.first; i != range.second; ++i)
		{
			// DEBUG_LOG << "Button " << _id << " releases instant " << instantEvent << '\n';
			i->second(this);
		}
		_instantReleaseQueue.erase(range.first, range.second);
		return true;
	}

	optional<Mapping> GetPressMapping()
	{
		if (!_keyToRelease)
		{
			// Look at active chord mappings starting with the latest activates chord
			for (auto activeChord = _context->chordStack.cbegin(); activeChord != _context->chordStack.cend(); activeChord++)
			{
				auto binding = _mapping.chordedValue(*activeChord);
				if (binding && *activeChord != _id)
				{
					_keyToRelease = *binding;
					_nameToRelease = _mapping.getName(*activeChord);
					return _keyToRelease;
				}
			}
			// Chord stack should always include NONE which will provide a value in the loop above
			throw runtime_error("ChordStack should always include ButtonID::NONE, for the chorded variable to return the base value.");
		}
		return _keyToRelease;
	}

	void RegisterInstant(BtnEvent evt, Callback cb) override
	{
		if (cb)
		{
			// DEBUG_LOG << "Button " << _id << " registers instant " << evt << '\n';
			_instantReleaseQueue.emplace(evt, cb);
		}
	}

	void ApplyGyroAction(KeyCode gyroAction) override
	{
		_context->gyroActionQueue.push_back({ _id, gyroAction });
	}

	void RemoveGyroAction() override
	{
		auto gyroAction = find_if(_context->gyroActionQueue.begin(), _context->gyroActionQueue.end(),
		  [this](auto pair)
		  {
			  // On a sim press, release the master button (the one who triggered the press)
			  return pair.first == (_masterPress ? _masterPress->_id : _id);
		  });
		if (gyroAction != _context->gyroActionQueue.end())
		{
			KeyCode key(gyroAction->second);
			ClearAllActiveToggle(key);
			for (auto currentlyActive = find_if(_context->gyroActionQueue.begin(), _context->gyroActionQueue.end(), bind(isSameKey, key, placeholders::_1));
			     currentlyActive != _context->gyroActionQueue.end();
			     currentlyActive = find_if(currentlyActive, _context->gyroActionQueue.end(), bind(isSameKey, key, placeholders::_1)))
			{
				// DEBUG_LOG << "Removing active gyro action for " << key.name << endl;
				currentlyActive = _context->gyroActionQueue.erase(currentlyActive);
			}
		}
	}

	void SetRumble(int smallRumble, int bigRumble) override
	{
		DEBUG_LOG << "Rumbling at " << smallRumble << " and " << bigRumble << '\n';
		_context->_rumble(smallRumble, bigRumble);
	}

	void ApplyBtnPress(KeyCode key) override
	{
		if (key.code >= X_UP && key.code <= X_START || key.code == PS_HOME || 
			key.code == PS_PAD_CLICK || key.code == X_LT || key.code == X_RT)
		{
			if (_context->_vigemController)
				_context->_vigemController->setButton(key, true);
		}
		else if (key.code == VK_NONAME)
		{
			if (_context->nn == 0)
				++_context->nn;
		}
		else if (key.code != NO_HOLD_MAPPED && HasActiveToggle(_context, key) == false)
		{
			pressKey(key, true);
		}
		DEBUG_LOG << "Pressing down on key " << key.name << endl;
	}

	void ApplyBtnRelease(KeyCode key) override
	{
		if (key.code >= X_UP && key.code <= X_START || key.code == PS_HOME ||
			key.code == PS_PAD_CLICK || key.code == X_LT || key.code == X_RT)
		{
			if (_context->_vigemController)
			{
				_context->_vigemController->setButton(key, false);
				ClearAllActiveToggle(key);
			}
		}
		else if (key.code != NO_HOLD_MAPPED)
		{
			pressKey(key, false);
			ClearAllActiveToggle(key);
		}
		DEBUG_LOG << "Releasing key " << key.name << endl;
	}

	void ApplyButtonToggle(KeyCode key, EventActionIf::Callback apply, EventActionIf::Callback release) override
	{
		auto currentlyActive = find_if(_context->activeTogglesQueue.begin(), _context->activeTogglesQueue.end(),
		  [this, key](pair<ButtonID, KeyCode> pair)
		  {
			  return pair.first == _id && pair.second == key;
		  });
		if (currentlyActive == _context->activeTogglesQueue.end())
		{
			DEBUG_LOG << "Adding active toggle for " << key.name << '\n';
			apply(this);
			_context->activeTogglesQueue.push_front({ _id, key });
		}
		else
		{
			release(this); // The bound action here should always erase the active toggle from the queue
		}
	}

	void ClearAllActiveToggle(KeyCode key)
	{
		for (auto currentlyActive = find_if(_context->activeTogglesQueue.begin(), _context->activeTogglesQueue.end(), bind(isSameKey, key, placeholders::_1));
		     currentlyActive != _context->activeTogglesQueue.end();
		     currentlyActive = find_if(currentlyActive, _context->activeTogglesQueue.end(), bind(isSameKey, key, placeholders::_1)))
		{
			DEBUG_LOG << "Removing active toggle for " << key.name << '\n';
			currentlyActive = _context->activeTogglesQueue.erase(currentlyActive);
		}
	}

	void StartCalibration() override
	{
		COUT << "Starting continuous calibration\n";
		_context->rightMainMotion->ResetContinuousCalibration();
		_context->rightMainMotion->StartContinuousCalibration();
		if (_context->leftMotion)
		{
			// Perform calibration on both gyros of a joycon pair regardless of mask
			_context->leftMotion->ResetContinuousCalibration();
			_context->leftMotion->StartContinuousCalibration();
		}
	}

	void FinishCalibration() override
	{
		_context->rightMainMotion->PauseContinuousCalibration();
		if (_context->leftMotion)
		{
			// Perform calibration on both gyros of a joycon pair regardless of mask
			_context->leftMotion->PauseContinuousCalibration();
		}
		COUT << "Gyro calibration set\n";
		ClearAllActiveToggle(KeyCode("CALIBRATE"));
	}

	const char *getDisplayName() override
	{
		return _nameToRelease.c_str();
	}
};

// Forward declare concrete states
class NoPress;
class BtnPress;
class TapPress;
class WaitSim;
class SimPressMaster;
class SimPressSlave;
class SimRelease;
class DiagPressSlave;
class DiagRelease;
class DblPressStart;
class DblPressNoPress;
class DblPressNoPressTap;
class DblPressNoPressHold;
class DblPressPress;
class InstRelease;

// Forward declare concrete nested states
class ActiveStartPress;
class ActiveHoldPress;

// Basic state react
void DigitalButtonState::react(OnEntry &e)
{
	// Uncomment below to diplay a log each time a button changes state
	// DEBUG_LOG << "Button " << pimpl()->_id << " is now in state " << _name << '\n';
}

// Basic Press reaction should be called in every concrete Press reaction
void DigitalButtonState::react(Pressed &e)
{
	pimpl()->_context->updateChordStack(true, pimpl()->_id);
}

// Basic Release reaction should be called in every concrete Release reaction
void DigitalButtonState::react(Released &e)
{
	pimpl()->_context->updateChordStack(false, pimpl()->_id);
}

void DigitalButtonState::react(chrono::steady_clock::time_point &e)
{
	// final implementation. All states can be assigned a new press time
	pimpl()->_press_times = e;
}

void DigitalButtonState::react(GetDuration &e)
{
	// final implementation. All states can be querried it's duration time.
	e.out_duration = pimpl()->GetPressDurationMS(e.in_now);
}


// Append to pocket_fsm macro
#define DB_CONCRETE_STATE(statename)           \
	CONCRETE_STATE(statename)                  \
	virtual BtnState getState() const override \
	{                                          \
		return BtnState::statename;            \
	}

// Base state for all nested states in which a mapping is active
class ActiveMappingState : public DigitalButtonState
{
public:
	virtual BtnState getState() const override
	{
		return BtnState::INVALID;
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnRelease, *pimpl());
	}

	REACT(Sync)
	final
	{
		Released rel{ e.pressTime, e.turboTime, e.holdTime };
		react(rel);
		// Redirect change of state to the caller of the Sync
		if (e.nextState == nullptr)
		{
			// Release from SimPress

			e.nextState = _nextState;
			_nextState = nullptr;
			changeState<SimRelease>();
		}
		else
		{
			if (e.activeMapping != nullptr)
			{
				// Activate Diagonal
				// DEBUG_LOG << "Button " << pimpl()->_id << " enables active diagonal as master\n";
				pimpl()->_masterPress = nullptr;
				pimpl()->_keyToRelease = *e.activeMapping;
				pimpl()->_nameToRelease = e.nameToRelease;
			}
			else // release diagonal
			{
				// DEBUG_LOG << "Button " << pimpl()->_id << " releases active diagonal\n";
				pimpl()->ClearKey();
			}
			pimpl()->_press_times = e.pressTime;
			delete _nextState;
			_nextState = e.nextState;
		}
	}
};

// Nested concrete states

class ActiveStartPress : public ActiveMappingState
{
	CONCRETE_STATE(ActiveStartPress)
	INITIAL_STATE(ActiveStartPress)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->GetPressMapping()->ProcessEvent(BtnEvent::OnPress, *pimpl());
	}

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);

		auto elapsed_time = pimpl()->GetPressDurationMS(e.time_now);
		if (elapsed_time > MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnPress);
		}
		if (elapsed_time > e.holdTime)
		{
			changeState<ActiveHoldPress>();
		}
	}

	REACT(Released)
	override
	{
		ActiveMappingState::react(e);
		pimpl()->_press_times = e.time_now; // Start counting tap duration
		changeState<TapPress>();
	}

};

class ActiveHoldPress : public ActiveMappingState
{
	CONCRETE_STATE(ActiveHoldPress)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnHold, *pimpl());
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnTurbo, *pimpl());
		pimpl()->_turboApplies++;
	}

	REACT(Pressed)
	override
	{
		auto elapsed_time = pimpl()->GetPressDurationMS(e.time_now);
		if (elapsed_time > e.holdTime + MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnHold);
		}
		if (floorf((elapsed_time - e.holdTime) / e.turboTime) >= pimpl()->_turboApplies)
		{
			pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnTurbo, *pimpl());
			pimpl()->_turboApplies++;
		}
		if (elapsed_time > e.holdTime + pimpl()->_turboReleases * e.turboTime + MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnTurbo);
			pimpl()->_turboReleases++;
		}
	}

	REACT(Released)
	override
	{
		ActiveMappingState::react(e);
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnHoldRelease, *pimpl());
		if (pimpl()->_instantReleaseQueue.empty())
		{
			changeState<NoPress>();
			pimpl()->ClearKey();
		}
		else
		{
			changeState<InstRelease>();
			pimpl()->_press_times = e.time_now; // Start counting tap duration
		}
	}
};

class DiagPressMaster : public pocket_fsm::NestedStateMachine<ActiveMappingState, DigitalButtonState>
{
	DB_CONCRETE_STATE(DiagPressMaster)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		initialize(new ActiveStartPress(_pimpl));
	}

	NESTED_REACT(Pressed)

	NESTED_REACT(Released)

	NESTED_REACT(Sync)

	void swapPimpl(DigitalButtonState &otherState) override
	{
		_currentState->resetPimpl(otherState);
		DigitalButtonState::swapPimpl(otherState);
	}
};

// Core concrete states

class NoPress : public DigitalButtonState
{
	DB_CONCRETE_STATE(NoPress)
	INITIAL_STATE(NoPress)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->_press_times = e.time_now;
		if (pimpl()->_mapping.hasSimMappings() && pimpl()->GetPressDurationMS(e.time_now) < SettingsManager::getV<float>(SettingID::SIM_PRESS_WINDOW)->value())
		{
			changeState<WaitSim>();
		}
		else if (pimpl()->_mapping.getDblPressMap())
		{
			// Start counting time between two start presses
			changeState<DblPressStart>();
		}
		else if (pimpl()->_mapping.hasDiagMappings())
		{
			size_t counter = 0;
			optional<MapIterator> diag = nullopt;
			for (auto btn = pimpl()->_context->_getMatchingDiagBtn(pimpl()->_id, diag); btn;
				 btn = pimpl()->_context->_getMatchingDiagBtn(pimpl()->_id, diag))
			{
				// DEBUG_LOG << "Button " << pimpl()->_id << " enables diagonal press with " << btn->_id << " who is in state " << btn->getCurrentStateName() << '\n';
				pimpl()->_masterPress = btn;
				pimpl()->_nameToRelease = pimpl()->_mapping.getDiagPressName((*diag)->first);
				pimpl()->_keyToRelease = (*diag)->second.value();
				Sync sync;
				sync.nameToRelease = pimpl()->_nameToRelease;
				sync.activeMapping = &*pimpl()->_keyToRelease;
				sync.pressTime = e.time_now;
				sync.holdTime = e.holdTime;
				sync.turboTime = e.turboTime;
				sync.dblPressWindow = e.dblPressWindow;
				sync.nextState = new DiagPressMaster();
				pimpl()->_masterPress->sendEvent(sync);
				++*diag;
				counter++;
			}

			if (counter > 0 )
				changeState<DiagPressSlave>();
			else
				changeState<BtnPress>();
		}
		else
		{
			changeState<BtnPress>();
		}
	}
};

class BtnPress : public pocket_fsm::NestedStateMachine<ActiveMappingState, DigitalButtonState>
{
	DB_CONCRETE_STATE(BtnPress)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		initialize(new ActiveStartPress(_pimpl));
	}

	NESTED_REACT(Pressed);
	NESTED_REACT(Released);
	NESTED_REACT(Sync);
};

class TapPress : public DigitalButtonState
{
	DB_CONCRETE_STATE(TapPress)

	REACT(OnEntry)
	override
	{
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnTap, *pimpl());
	}

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->ReleaseInstant(BtnEvent::OnRelease);
		pimpl()->ReleaseInstant(BtnEvent::OnTap);
		changeState<BtnPress>();
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnRelease);
			pimpl()->ReleaseInstant(BtnEvent::OnTap);
		}
		if (!pimpl()->_keyToRelease || pimpl()->GetPressDurationMS(e.time_now) > pimpl()->_keyToRelease->getTapDuration())
		{
			changeState<NoPress>();
		}
	}

	REACT(OnExit)
	override
	{
		pimpl()->_keyToRelease->ProcessEvent(BtnEvent::OnTapRelease, *pimpl());
		pimpl()->ClearKey();
	}
};

class SimPressMaster : public pocket_fsm::NestedStateMachine<ActiveMappingState, DigitalButtonState>
{
	DB_CONCRETE_STATE(SimPressMaster)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		initialize(new ActiveStartPress(_pimpl));
	}

	NESTED_REACT(Pressed)

	NESTED_REACT(Released)

	NESTED_REACT(Sync)
};

class SimPressSlave : public DigitalButtonState
{
	DB_CONCRETE_STATE(SimPressSlave)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->_masterPress->getState() != BtnState::SimPressMaster)
		{
			// The master button has released! change state now!
			changeState<SimRelease>();
			pimpl()->_masterPress = nullptr;
		}
		// else do nothing
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->_masterPress->getState() != BtnState::SimPressMaster)
		{
			// The master button has released! change state now!
			changeState<SimRelease>();
			pimpl()->_masterPress = nullptr;
		}
		else
		{
			// Process at the master's end
			Sync sync;
			sync.pressTime = e.time_now;
			sync.holdTime = e.holdTime;
			sync.turboTime = e.turboTime;
			sync.dblPressWindow = e.dblPressWindow;
			_nextState = pimpl()->_masterPress->sendEvent(sync).nextState;
		}
	}
};

class WaitSim : public DigitalButtonState
{
	DB_CONCRETE_STATE(WaitSim)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		// Is there a sim mapping on this button where the other button is in WaitSim state too?
		auto simBtn = pimpl()->_context->_getMatchingSimBtn(pimpl()->_id);
		if (simBtn)
		{
			changeState<SimPressSlave>();
			pimpl()->_press_times = e.time_now;                                          // reset Timer
			pimpl()->_keyToRelease = pimpl()->_mapping.atSimPress(simBtn->_id)->value(); // Make a copy
			pimpl()->_nameToRelease = pimpl()->_mapping.getSimPressName(simBtn->_id);
			pimpl()->_masterPress = simBtn; // Second to press is the slave

			Sync sync;
			sync.nextState = new SimPressMaster();
			sync.pressTime = e.time_now;
			sync.activeMapping = &*pimpl()->_keyToRelease;
			sync.nameToRelease = pimpl()->_nameToRelease;
			sync.dblPressWindow = e.dblPressWindow;
			simBtn->sendEvent(sync);
		}
		else if (pimpl()->GetPressDurationMS(e.time_now) > SettingsManager::getV<float>(SettingID::SIM_PRESS_WINDOW)->value())
		{
			// Button is still pressed but Sim delay did expire
			if (pimpl()->_mapping.getDblPressMap())
			{
				// Start counting time between two start presses
				changeState<DblPressStart>();
			}
			else if (pimpl()->_mapping.hasDiagMappings())
			{
				size_t counter = 0;
				optional<MapIterator> diag = nullopt;
				for (auto btn = pimpl()->_context->_getMatchingDiagBtn(pimpl()->_id, diag); btn;
				     btn = pimpl()->_context->_getMatchingDiagBtn(pimpl()->_id, diag))
				{
					// DEBUG_LOG << "Button " << pimpl()->_id << " enables diagonal press with " << btn->_id << " who is in state " << btn->getCurrentStateName() << '\n';
					pimpl()->_masterPress = btn;
					pimpl()->_nameToRelease = pimpl()->_mapping.getDiagPressName((*diag)->first);
					pimpl()->_keyToRelease = (*diag)->second.value();
					Sync sync;
					sync.nameToRelease = pimpl()->_nameToRelease;
					sync.activeMapping = &*pimpl()->_keyToRelease;
					sync.pressTime = e.time_now;
					sync.holdTime = e.holdTime;
					sync.turboTime = e.turboTime;
					sync.dblPressWindow = e.dblPressWindow;
					sync.nextState = new DiagPressMaster();
					pimpl()->_masterPress->sendEvent(sync);
					++*diag;
					counter++;
				}

				if (counter > 0)
					changeState<DiagPressSlave>();
				else
					changeState<BtnPress>();
			}
			else // Handle regular press mapping
			{
				changeState<BtnPress>();
				// _press_times = _timeNow;
			}
		}
		// Else let time flow, stay in this state, no output.
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		// Button was released before sim delay expired
		if (pimpl()->_mapping.getDblPressMap())
		{
			// Start counting time between two start presses
			changeState<DblPressStart>();
		}
		else
		{
			changeState<BtnPress>();
		}
	}

	REACT(Sync)
	override
	{
		pimpl()->_masterPress = nullptr;
		pimpl()->_press_times = e.pressTime;
		pimpl()->_keyToRelease = *e.activeMapping;
		pimpl()->_nameToRelease = e.nameToRelease;
		_nextState = e.nextState; // changeState <typeof(e.nextState)> () 
	}
};

class SimRelease : public DigitalButtonState
{
	DB_CONCRETE_STATE(SimRelease)

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		changeState<NoPress>();
		pimpl()->ClearKey();
	}

	REACT(Sync)
	final
	{
		if (e.nextState != nullptr && e.activeMapping != nullptr)
		{
			Released rel{ e.pressTime, e.turboTime, e.holdTime };
			react(rel);
			// Redirect change of state to the caller of the Sync

			// Activate Diagonal
			// DEBUG_LOG << "Button " << pimpl()->_id << " enables active diagonal as master\n";
			pimpl()->_masterPress = nullptr;
			pimpl()->_keyToRelease = *e.activeMapping;
			pimpl()->_nameToRelease = e.nameToRelease;
			pimpl()->_press_times = e.pressTime;
			delete _nextState;
			_nextState = e.nextState;
		}
	}
};



class DiagPressSlave : public DigitalButtonState
{
	DB_CONCRETE_STATE(DiagPressSlave)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);

		if (!pimpl()->_masterPress || pimpl()->_masterPress->getState() != BtnState::DiagPressMaster)
		{
			// Master has released me!
			pimpl()->_masterPress = nullptr;
			pimpl()->ClearKey();
			changeState<NoPress>();
		}
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->_masterPress && pimpl()->_masterPress->getState() == BtnState::DiagPressMaster)
		{
			// Inform Diagonal Master of the release
			// Here we're swapping the current state of the master and slave buttons. This enables the released button
			// to process taps and instants whereas the other button can process its own binding activation.
			optional<MapIterator> it;
			auto me = pimpl()->_context->_getMatchingDiagBtn(pimpl()->_masterPress->_id, it);
			if (me)
			{
				// DEBUG_LOG << pimpl()->_id << " is performing the swap!\n";
				pimpl()->_masterPress->swapState(*me);
				//DEBUG_LOG << pimpl()->_id << " is now in state " << getState() << " with mapping " << pimpl()->_nameToRelease << " set to " << pimpl()->_mapping.value() << '\n';
				//DEBUG_LOG << pimpl()->_masterPress->_id << " is now in state " << pimpl()->_masterPress->getState() << '\n';
			}
			else
			{
				CERR << "I can't find myself as the other diagonal?!?";
			}
		}
		else 
		{
			pimpl()->_masterPress = nullptr;
			pimpl()->ClearKey();
			changeState<NoPress>();
		}
	}
};

class DblPressStart : public pocket_fsm::NestedStateMachine<ActiveMappingState, DigitalButtonState>
{
	DB_CONCRETE_STATE(DblPressStart)
	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		initialize(new ActiveStartPress(_pimpl));
	}

	NESTED_REACT(Pressed);

	REACT(Released)
	override
	{
		sendEvent(e);
		if (_nextState)
		{
			if (_nextState->_name == "NoPress")
			{
				delete _nextState;
				_nextState = nullptr;
				changeState<DblPressNoPress>(_onTransition);
			}
			else if (_nextState->_name == "TapPress")
			{
				delete _nextState;
				_nextState = nullptr;
				changeState<DblPressNoPressTap>(_onTransition);
			}
		}
	}
};

class DblPressNoPress : public DigitalButtonState
{
	DB_CONCRETE_STATE(DblPressNoPress)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			pimpl()->_press_times = e.time_now; // reset Timer to raise a tap
			changeState<BtnPress>();
		}
		else
		{
			pimpl()->_press_times = e.time_now;
			changeState<DblPressPress>();
		}
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnRelease);
		}

		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			changeState<NoPress>();
		}
	}
};

class DblPressNoPressTap : public DigitalButtonState
{
	DB_CONCRETE_STATE(DblPressNoPressTap)
	REACT(Pressed)
	override
	{
		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			pimpl()->_press_times = e.time_now; // reset Timer to raise a tap
			changeState<TapPress>();
		}
		else
		{
			pimpl()->_keyToRelease = pimpl()->_mapping.getDblPressMap()->second;
			pimpl()->_nameToRelease = pimpl()->_mapping.getName(pimpl()->_id);
			pimpl()->_press_times = e.time_now;
			changeState<DblPressPress>();
		}
	}

	REACT(Released)
	override
	{
		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			pimpl()->_press_times = e.time_now; // reset Timer to raise a tap
			changeState<TapPress>();
		}
	}
};

class DblPressNoPressHold : public DigitalButtonState
{
	DB_CONCRETE_STATE(DblPressNoPressHold)
	REACT(Pressed)
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			changeState<BtnPress>();
			// Don't reset timer to preserve hold press behaviour
			pimpl()->GetPressMapping()->ProcessEvent(BtnEvent::OnPress, *pimpl());
		}
		else
		{
			changeState<DblPressPress>();
			pimpl()->_press_times = e.time_now;
			pimpl()->_keyToRelease = pimpl()->_mapping.getDblPressMap()->second;
			pimpl()->_nameToRelease = pimpl()->_mapping.getName(pimpl()->_id);
		}
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > e.dblPressWindow)
		{
			changeState<BtnPress>();
			// Don't reset timer to preserve hold press behaviour
		}
	}
};

class DblPressPress : public pocket_fsm::NestedStateMachine<ActiveMappingState, DigitalButtonState>
{
	DB_CONCRETE_STATE(DblPressPress)

	REACT(OnEntry)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->_keyToRelease = pimpl()->_mapping.getDblPressMap()->second;
		pimpl()->_nameToRelease = pimpl()->_mapping.getName(pimpl()->_id);
		initialize(new ActiveStartPress(_pimpl));
	}

	NESTED_REACT(Pressed);
	NESTED_REACT(Released);
};

class InstRelease : public DigitalButtonState
{
	DB_CONCRETE_STATE(InstRelease)

	REACT(Pressed)
	override
	{
		DigitalButtonState::react(e);
		pimpl()->ReleaseInstant(BtnEvent::OnRelease);
		pimpl()->ClearKey();
		changeState<NoPress>();
	}

	REACT(Released)
	override
	{
		DigitalButtonState::react(e);
		if (pimpl()->GetPressDurationMS(e.time_now) > MAGIC_INSTANT_DURATION)
		{
			pimpl()->ReleaseInstant(BtnEvent::OnRelease);
			pimpl()->ClearKey();
			changeState<NoPress>();
		}
	}
};

// Top level interface

DigitalButton::DigitalButton(shared_ptr<DigitalButton::Context> _context, JSMButton &mapping)
  : _id(mapping._id)
{
	initialize(new NoPress(new DigitalButtonImpl(mapping, _context)));
}

DigitalButton::Context::Context(Gamepad::Callback virtualControllerCallback, shared_ptr<MotionIf> mainMotion)
  : rightMainMotion(mainMotion)
{
	chordStack.push_front(ButtonID::NONE); // Always hold mapping none at the end to _handle modeshifts and chords
#ifdef _WIN32
	auto virtual_controller = SettingsManager::getV<ControllerScheme>(SettingID::VIRTUAL_CONTROLLER);
	if (virtual_controller->value() != ControllerScheme::NONE)
	{
		_vigemController.reset(Gamepad::getNew(virtual_controller->value(), virtualControllerCallback));
		string error;
		if (!_vigemController->isInitialized(&error))
		{
			virtual_controller->set(ControllerScheme::NONE);
		}
		if (!error.empty())
		{
			CERR << error << '\n';
		}
	}
#endif
}
