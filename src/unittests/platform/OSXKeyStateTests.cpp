/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXKeyStateTests.h"

#include "base/EventQueue.h"
#include "deskflow/unix/AppUtilUnix.h"
#include <ApplicationServices/ApplicationServices.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>
#include <algorithm>
#include <deque>

#define SHIFT_ID_L kKeyShift_L
#define SHIFT_ID_R kKeyShift_R
#define SHIFT_BUTTON 57
#define A_CHAR_ID 0x00000061
#define A_CHAR_BUTTON 001

void OSXKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void OSXKeyStateTests::mapModifiersFromOSX_OSXMask()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);

  KeyModifierMask outMask = 0;

  uint32_t shiftMask = 0 | kCGEventFlagMaskShift;
  outMask = keyState.mapModifiersFromOSX(shiftMask);
  QCOMPARE(outMask, KeyModifierShift);

  uint32_t ctrlMask = 0 | kCGEventFlagMaskControl;
  outMask = keyState.mapModifiersFromOSX(ctrlMask);
  QCOMPARE(outMask, KeyModifierControl);

  uint32_t altMask = 0 | kCGEventFlagMaskAlternate;
  outMask = keyState.mapModifiersFromOSX(altMask);
  QCOMPARE(outMask, KeyModifierAlt);

  uint32_t cmdMask = 0 | kCGEventFlagMaskCommand;
  outMask = keyState.mapModifiersFromOSX(cmdMask);
  QCOMPARE(outMask, KeyModifierSuper);

  uint32_t capsMask = 0 | kCGEventFlagMaskAlphaShift;
  outMask = keyState.mapModifiersFromOSX(capsMask);
  QCOMPARE(outMask, KeyModifierCapsLock);

  uint32_t numMask = 0 | kCGEventFlagMaskNumericPad;
  outMask = keyState.mapModifiersFromOSX(numMask);
  QCOMPARE(outMask, KeyModifierNumLock);
}

void OSXKeyStateTests::fakePollShift()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(SHIFT_ID_L, 0, 1, "en");
  QVERIFY(isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyDown(SHIFT_ID_R, 0, 2, "en");
  QVERIFY(isKeyPressed(keyState, SHIFT_BUTTON));

  keyState.fakeKeyUp(2);
  QVERIFY(!isKeyPressed(keyState, SHIFT_BUTTON));
}

void OSXKeyStateTests::fakePollChar()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(A_CHAR_ID, 0, 1, "en");
  QVERIFY(isKeyPressed(keyState, A_CHAR_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, A_CHAR_BUTTON));

  // HACK: delete the key in case it was typed into a text editor.
  // we should really set focus to an invisible window.
  keyState.fakeKeyDown(kKeyBackSpace, 0, 2, "en");
  keyState.fakeKeyUp(2);
}

void OSXKeyStateTests::fakePollCharWithModifier()
{
  deskflow::KeyMap keyMap;
  EventQueue eventQueue;
  OSXKeyState keyState(&eventQueue, keyMap, {"en"}, true);
  keyState.updateKeyMap();

  keyState.fakeKeyDown(A_CHAR_ID, KeyModifierShift, 1, "en");
  QVERIFY(isKeyPressed(keyState, A_CHAR_BUTTON));

  keyState.fakeKeyUp(1);
  QVERIFY(!isKeyPressed(keyState, A_CHAR_BUTTON));

  // HACK: delete the key in case it was typed into a text editor.
  // we should really set focus to an invisible window.
  keyState.fakeKeyDown(kKeyBackSpace, 0, 2, "en");
  keyState.fakeKeyUp(2);
}

bool OSXKeyStateTests::isKeyPressed(const OSXKeyState &keyState, KeyButton button)
{
  // HACK: allow os to realize key state changes.
  Arch::sleep(.2);

  IKeyState::KeyButtonSet pressed;
  keyState.pollPressedKeys(pressed);

  IKeyState::KeyButtonSet::const_iterator it;
  for (it = pressed.begin(); it != pressed.end(); ++it) {
    LOG_DEBUG("checking key %d", *it);
    if (*it == button) {
      return true;
    }
  }
  return false;
}

void OSXKeyStateTests::audit_nativeTyping_data()
{
  QTest::addColumn<int>("gapMs");
  QTest::addColumn<bool>("keypad");
  QTest::newRow("letters-10ms") << 10 << false;
  QTest::newRow("letters-1ms") << 1 << false;
  QTest::newRow("keypad-10ms") << 10 << true;
  QTest::newRow("keypad-1ms") << 1 << true;
}

// Opt-in: uses a dedicated focused receiver, never an existing user document.
void OSXKeyStateTests::audit_nativeTyping()
{
  if (!qEnvironmentVariableIsSet("DESKFLOW_AUDIT_LIVE"))
    QSKIP("Native delivery audit requires explicit opt-in");
  if (!AXIsProcessTrusted())
    QSKIP("Audit executable lacks macOS Accessibility permission");
  static bool focusLost = false;
  if (focusLost)
    QSKIP("Earlier receiver lost focus; remaining live tests cancelled");
  QFETCH(int, gapMs);
  QFETCH(bool, keypad);
  m_log.setFilter(LogLevel::Level::Info);
  class Receiver : public QLineEdit
  {
  public:
    QElapsedTimer clock;
    std::deque<qint64> pending;
    std::vector<double> latencyMs;
    void keyPressEvent(QKeyEvent *event) override
    {
      if (!event->text().isEmpty() && !pending.empty()) {
        latencyMs.push_back((clock.nsecsElapsed() - pending.front()) / 1e6);
        pending.pop_front();
      }
      QLineEdit::keyPressEvent(event);
    }
  } receiver;
  receiver.clock.start();
  receiver.setWindowTitle("Deskflow audit — controlled typing receiver");
  receiver.resize(700, 80);
  receiver.show();
  receiver.raise();
  receiver.activateWindow();
  receiver.setFocus();
  QVERIFY(QTest::qWaitForWindowActive(&receiver, 5000));
  EventQueue events;
  AppUtilUnix appUtil(&events);
  const auto language = appUtil.getCurrentLanguageCode();
  OSXKeyState state(&events, appUtil.getKeyboardLayoutList(), false);
  state.updateKeyMap();
  state.updateKeyState();
  QString expected;
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < 500; ++i) {
    if (!receiver.isActiveWindow()) {
      focusLost = true;
      QFAIL("Receiver lost focus; input audit stopped");
    }
    receiver.pending.push_back(receiver.clock.nsecsElapsed());
    state.fakeKeyDown(keypad ? kKeyKP_1 : 'a', 0, 1, language);
    state.fakeKeyUp(1);
    expected += keypad ? '1' : 'a';
    QEventLoop delivery;
    QTimer::singleShot(gapMs, &delivery, &QEventLoop::quit);
    delivery.exec();
  }
  const auto deliveryMs = timer.elapsed();
  QTest::qWait(500);
  qInfo("500 down/up pairs, including requested gaps and event processing: %lld ms", deliveryMs);
  qInfo("Native receiver: expected=%lld actual=%lld", expected.size(), receiver.text().size());
  QCOMPARE(receiver.text(), expected);
  QCOMPARE(receiver.latencyMs.size(), size_t(500));
  std::sort(receiver.latencyMs.begin(), receiver.latencyMs.end());
  qInfo(
      "inject-to-Qt delivery ms: p50=%.3f p95=%.3f p99=%.3f max=%.3f", receiver.latencyMs[249], receiver.latencyMs[474],
      receiver.latencyMs[494], receiver.latencyMs.back()
  );
}

QTEST_MAIN(OSXKeyStateTests)
