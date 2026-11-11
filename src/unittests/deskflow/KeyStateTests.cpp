/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeyStateTests.h"
#include "base/EventQueue.h"
#include "deskflow/KeyMap.h"
#include "deskflow/KeypadPolicy.h"
#include "deskflow/ScrollAnimation.h"

#include "MockEventQueue.h"
#include "MockKeyMap.h"
#include "MockKeyState.h"

#include "deskflow/ISecondaryComputer.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <algorithm>

namespace {

//! KeyState that records the keystrokes it is asked to synthesize.
class RecordingKeyState : public KeyState
{
public:
  RecordingKeyState(IEventQueue *events, deskflow::KeyMap &keyMap, std::vector<std::string> layouts, bool langSync)
      : KeyState(events, keyMap, std::move(layouts), langSync)
  {
  }

  int32_t pollActiveGroup() const override
  {
    return m_activeGroup;
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return 0;
  }
  bool fakeCtrlAltDel() override
  {
    return false;
  }
  void getKeyMap(deskflow::KeyMap &) override
  {
  }
  bool fakeMediaKey(KeyID) override
  {
    return false;
  }
  void pollPressedKeys(KeyButtonSet &) const override
  {
  }
  void fakeKey(const Keystroke &keystroke) override
  {
    m_faked.push_back(keystroke);
  }

  int countStrokes(Keystroke::KeyType type) const
  {
    return static_cast<int>(std::count_if(m_faked.begin(), m_faked.end(), [type](const Keystroke &k) {
      return k.m_type == type;
    }));
  }

  int32_t m_activeGroup = 0;
  std::vector<Keystroke> m_faked;
};

//! Two groups on one button: a latin key in group 0 ("en"), a thai key in group 1 ("th").
void buildTwoGroupKeyMap(deskflow::KeyMap &keyMap, KeyID enKey, KeyID thKey, KeyButton button)
{
  deskflow::KeyMap::KeyItem item;
  item.m_button = button;

  item.m_id = enKey;
  item.m_group = 0;
  keyMap.addKeyEntry(item);

  item.m_id = thKey;
  item.m_group = 1;
  keyMap.addKeyEntry(item);

  keyMap.finish();
}

constexpr KeyID kLatinA = 'a';
constexpr KeyID kThaiFoFan = 0x0e1f; // ฟ, the same physical key as 'a' on a thai layout
constexpr KeyButton kSharedButton = 1;

} // namespace

void KeyStateTests::initTestCase()
{
  m_arch.init();
}

void KeyStateTests::keyDown()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  keyState.onKey(1, true, KeyModifierAlt);

  QVERIFY(keyState.getKeyState(1));
}

void KeyStateTests::keyUp()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);
  QVERIFY(!keyState.getKeyState(1));
}

void KeyStateTests::invalidKey()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(0, true, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(0));
}

void KeyStateTests::onKey_aKeyDown_keyStateOne()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(1, true, KeyModifierAlt);

  QVERIFY(keyState.getKeyState(1));
}

void KeyStateTests::onKey_aKeyUp_keyStateZero()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(1, false, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(1));
}

void KeyStateTests::onKey_invalidKey_keyStateZero()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.onKey(0, true, KeyModifierAlt);

  QVERIFY(!keyState.getKeyState(0));
}

void KeyStateTests::updateKeyState_pollDoesNothing_keyNotSet()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.updateKeyState();

  QVERIFY(!keyState.isKeyDown(1));
}

void KeyStateTests::updateKeyState_activeModifiers_maskNotSet()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  keyState.updateKeyState();

  QCOMPARE(0, keyState.getActiveModifiers());
}

void KeyStateTests::fakeKeyRepeat_invalidKey_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.fakeKeyRepeat(0, 0, 0, 0, "en"));
}

void KeyStateTests::fakeKeyUp_buttonNotDown_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.fakeKeyUp(0));
}

void KeyStateTests::isKeyDown_noKeysDown_returnsFalse()
{
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, m_keymap);

  QVERIFY(!keyState.isKeyDown(1));
}

void KeyStateTests::isKeyDown_keyDown_retrunsTrue()
{
  MockKeyMap keyMap;
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  deskflow::KeyMap::KeyItem key;
  key.m_button = 1;
  keyState.fakeKeyDown(1, 0, 1, "en");

  QVERIFY(keyState.isKeyDown(1));
}

void KeyStateTests::updateKeyState_pollInsertsSingleKey_keyIsDown()
{
  MockKeyMap keyMap;
  MockEventQueue eventQueue;
  MockKeyState keyState(eventQueue, keyMap);

  deskflow::KeyMap::KeyItem key;
  key.m_button = 1;
  keyState.fakeKeyDown(1, 0, 1, "en");

  keyState.updateKeyState();
  QVERIFY(keyState.isKeyDown(1));
}

void KeyStateTests::fakeKeyDown_langSyncEnabled_switchesToServerGroup()
{
  MockEventQueue eventQueue;
  deskflow::KeyMap keyMap;
  buildTwoGroupKeyMap(keyMap, kLatinA, kThaiFoFan, kSharedButton);

  RecordingKeyState keyState(&eventQueue, keyMap, {"en", "th"}, true);
  keyState.fakeKeyDown(kThaiFoFan, 0, kSharedButton, "th");

  QCOMPARE(keyState.countStrokes(deskflow::KeyMap::Keystroke::KeyType::Group), 1);
  const auto group = std::find_if(keyState.m_faked.begin(), keyState.m_faked.end(), [](const auto &k) {
    return k.m_type == deskflow::KeyMap::Keystroke::KeyType::Group;
  });
  QCOMPARE(group->m_data.m_group.m_group, 1);
}

void KeyStateTests::fakeKeyDown_langSyncDisabled_keepsLocalGroup()
{
  MockEventQueue eventQueue;
  deskflow::KeyMap keyMap;
  buildTwoGroupKeyMap(keyMap, kLatinA, kThaiFoFan, kSharedButton);

  RecordingKeyState keyState(&eventQueue, keyMap, {"en", "th"}, false);
  keyState.fakeKeyDown(kThaiFoFan, 0, kSharedButton, "th");

  // the local layout must be left alone, but the key itself is still pressed by position
  QCOMPARE(keyState.countStrokes(deskflow::KeyMap::Keystroke::KeyType::Group), 0);
  QVERIFY(keyState.countStrokes(deskflow::KeyMap::Keystroke::KeyType::Button) > 0);
  QCOMPARE(keyState.m_faked.front().m_data.m_button.m_button, kSharedButton);
}

void KeyStateTests::audit_100000Transitions()
{
  deskflow::KeyMap map;
  buildTwoGroupKeyMap(map, kLatinA, kThaiFoFan, kSharedButton);
  EventQueue events;
  RecordingKeyState state(&events, map, {"en", "th"}, false);
  state.m_faked.reserve(100000);
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < 50000; ++i) {
    state.fakeKeyDown(kLatinA, 0, kSharedButton, "en");
    QVERIFY(state.fakeKeyUp(kSharedButton));
  }
  qInfo("100000 mapped transitions: %lld ns (recording backend, not OS injection)", timer.nsecsElapsed());
  QCOMPARE(state.countStrokes(deskflow::KeyMap::Keystroke::KeyType::Button), 100000);
  for (size_t i = 0; i < state.m_faked.size(); ++i) {
    QCOMPARE(state.m_faked[i].m_data.m_button.m_press, i % 2 == 0);
  }
}

void KeyStateTests::audit_scrollChunking()
{
  class Sink : public ISecondaryComputer
  {
  public:
    void fakeMouseButton(ButtonID, bool) override
    {
    }
    void fakeMouseMove(int32_t, int32_t) override
    {
    }
    void fakeMouseRelativeMove(int32_t, int32_t) const override
    {
    }
    void fakeMouseWheel(ScrollDelta) const override
    {
    }
  };
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto previous = Settings::settingsFile();
  Settings::setSettingsFile(dir.filePath("audit.ini"));
  for (double scale : {0.1, 0.25, 1.0, 10.0}) {
    Settings::setValue(Settings::Client::YScrollScale, scale);
    Settings::setValue(Settings::Client::XScrollScale, scale);
    for (bool invert : {false, true}) {
      Settings::setValue(Settings::Client::InvertYScroll, invert);
      Settings::setValue(Settings::Client::InvertXScroll, invert);
      for (double divisor : {1.0, 4.0, 120.0}) {
        Sink sink, grouped;
        int x = 0, y = 0;
        for (int i = 0; i < 1200; ++i) {
          const auto delta = sink.applyScrollModifier({-1, 1}, divisor);
          x += delta.x;
          y += delta.y;
        }
        const auto total = grouped.applyScrollModifier({-1200, 1200}, divisor);
        // Floating point accumulation may retain one final sub-unit; total error is bounded.
        QVERIFY(std::abs(x - total.x) <= 1);
        QVERIFY(std::abs(y - total.y) <= 1);
        const auto reverse = sink.applyScrollModifier({1200, -1200}, divisor);
        QVERIFY(std::abs(x + reverse.x) <= 1);
        QVERIFY(std::abs(y + reverse.y) <= 1);
        sink.resetScrollRemainder();
        QCOMPARE(sink.applyScrollModifier({0, 0}, divisor).y, 0);
      }
    }
  }
  Settings::setSettingsFile(previous);
}

void KeyStateTests::audit_hotkeyRollover()
{
  deskflow::KeyMap map;
  for (int i = 1; i <= 2; ++i) {
    deskflow::KeyMap::KeyItem item;
    item.m_id = kKeyKP_0 + i;
    item.m_button = i;
    map.addKeyEntry(item);
  }
  map.finish();
  EventQueue events;
  RecordingKeyState state(&events, map, {"en"}, false);
  state.fakeKeyDown(keypadDigits(kKeyKP_End), 0, 87, "en");
  state.fakeKeyDown(keypadDigits(kKeyKP_Down), 0, 88, "en");
  QVERIFY(state.fakeKeyUp(87));
  QVERIFY(state.fakeKeyUp(88));
  QCOMPARE(state.countStrokes(deskflow::KeyMap::Keystroke::KeyType::Button), 4);
  QCOMPARE(keypadDigits(kKeyLeft), kKeyLeft);
}

void KeyStateTests::smoothWheelAnimation()
{
  ScrollAnimation animation;
  animation.add({-30, 30}, 0);
  int x = 0, y = 0, frames = 0;
  for (int i = 1; i <= 13; ++i) {
    auto delta = animation.step(i / 120.0);
    x += delta.x;
    y += delta.y;
    if (delta.y)
      ++frames;
  }
  QCOMPARE(x, -30);
  QCOMPARE(y, 30);
  QVERIFY(frames >= 8);
  QVERIFY(!animation.active());
  animation.add({0, 30}, 1);
  QVERIFY(animation.step(1.02).y > 0);
  animation.add({0, -30}, 1.02);
  QVERIFY(animation.step(1.03).y < 0);
  animation.reset();
  QCOMPARE(animation.step(2).y, 0);
  for (int i = 0; i < 100; ++i)
    animation.add({0, 1}, 3);
  QCOMPARE(animation.step(3.2).y, 100);
  QVERIFY(!animation.active());
  // Input arriving just after a frame must retain its entire distance.
  animation.add({0, 30}, 4);
  int total = animation.step(4.002).y;
  animation.add({0, 30}, 4.003);
  total += animation.step(4.2).y;
  QCOMPARE(total, 60);
  // Interleave sustained high-rate input and frames, then drain a delayed frame.
  total = 0;
  for (int i = 0; i < 1000; ++i) {
    animation.add({0, 1}, 5 + i * 0.0001);
    if (i % 83 == 0)
      total += animation.step(5 + i * 0.0001).y;
  }
  total += animation.step(6).y;
  QCOMPARE(total, 1000);
  QVERIFY(!animation.active());
}

QTEST_MAIN(KeyStateTests)
