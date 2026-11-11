/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXKeyStateTests.h"
#include "platform/OSXComputer.h"
#include <QCursor>
#include <QWheelEvent>

#include "base/EventQueue.h"
#include "deskflow/KeypadPolicy.h"
#include "deskflow/unix/AppUtilUnix.h"
#include <ApplicationServices/ApplicationServices.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScopeGuard>
#include <QTemporaryDir>
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

void OSXKeyStateTests::audit_nativeScroll()
{
  if (!qEnvironmentVariableIsSet("DESKFLOW_AUDIT_LIVE"))
    QSKIP("Native input test requires dedicated receiver and DESKFLOW_AUDIT_LIVE=1");
  class Receiver : public QWidget
  {
  public:
    int pixels = 0, frames = 0, ended = 0;
    void wheelEvent(QWheelEvent *event) override
    {
      qInfo() << "wheel" << event->pixelDelta() << event->phase();
      pixels += event->pixelDelta().y();
      frames += event->pixelDelta().y() != 0;
      ended += event->phase() == Qt::ScrollEnd;
      event->accept();
    }
  } receiver;
  receiver.setWindowTitle("Deskflow smooth wheel test");
  receiver.resize(600, 300);
  receiver.show();
  receiver.raise();
  receiver.activateWindow();
  QVERIFY(QTest::qWaitForWindowActive(&receiver, 5000));
  const auto oldCursor = QCursor::pos();
  auto restore = qScopeGuard([&] { QCursor::setPos(oldCursor); });
  QCursor::setPos(receiver.mapToGlobal(receiver.rect().center()));
  EventQueue events;
  AppUtilUnix appUtil(&events);
  events.addEvent(Event(EventTypes::Quit));
  events.loop();
  QTemporaryDir settingsDir;
  QVERIFY(settingsDir.isValid());
  const auto oldSettings = Settings::settingsFile();
  Settings::setSettingsFile(settingsDir.filePath("scroll.ini"));
  auto restoreSettings = qScopeGuard([&] { Settings::setSettingsFile(oldSettings); });
  Settings::setValue(Settings::Client::YScrollScale, 1.0);
  OSXComputer computer(&events, false);

  auto pump = [&](int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
      Event event;
      if (events.getEvent(event, 0)) {
        events.dispatchEvent(event);
        Event::deleteData(event);
      }
      QCoreApplication::processEvents();
      QTest::qWait(1);
    }
  };
  pump(30);
  receiver.raise();
  receiver.activateWindow();
  QCursor::setPos(receiver.mapToGlobal(receiver.rect().center()));
  QVERIFY(AXIsProcessTrusted());
  const auto center = QCursor::pos();
  auto move =
      CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(center.x(), center.y()), kCGMouseButtonLeft);
  CGEventPost(kCGHIDEventTap, move);
  CFRelease(move);
  pump(350);
  computer.fakeMouseWheel({0, 120});
  pump(250);
  qInfo() << "Native scroll pixels/frames/end:" << receiver.pixels << receiver.frames << receiver.ended;
  QCOMPARE(receiver.pixels, 30);
  QVERIFY(receiver.frames >= 6);
  QVERIFY(receiver.ended >= 1);
  receiver.pixels = receiver.frames = receiver.ended = 0;
  for (int i = 0; i < 10; ++i) {
    computer.fakeMouseWheel({0, -120});
    pump(15);
  }
  pump(250);
  QCOMPARE(receiver.pixels, -300);
  QVERIFY(receiver.frames >= 15);
  QVERIFY(receiver.ended >= 1);
}

void OSXKeyStateTests::audit_nativeMixed_data()
{
  QTest::addColumn<int>("scenario");
  QTest::newRow("mixed-text-overlap") << 0;
  QTest::newRow("keypad-rollover-repeat") << 1;
  QTest::newRow("shift-release-before-letter") << 2;
  QTest::newRow("held-key-repeat") << 3;
  QTest::newRow("command-shortcut-release") << 4;
}

void OSXKeyStateTests::audit_nativeMixed()
{
  if (!qEnvironmentVariableIsSet("DESKFLOW_AUDIT_LIVE") || !AXIsProcessTrusted())
    QSKIP("Requires native receiver opt-in and Accessibility permission");
  QFETCH(int, scenario);
  m_log.setFilter(LogLevel::Level::Info);
  class MixedReceiver : public QLineEdit
  {
  public:
    int repeats = 0, presses = 0, empty = 0;
    void keyPressEvent(QKeyEvent *event) override
    {
      ++presses;
      if (event->isAutoRepeat())
        ++repeats;
      if (event->text().isEmpty())
        ++empty;
      QLineEdit::keyPressEvent(event);
    }
  } receiver;
  receiver.setWindowTitle("Deskflow mixed typing and rollover test");
  receiver.resize(800, 100);
  receiver.show();
  receiver.raise();
  receiver.activateWindow();
  receiver.setFocus();
  QVERIFY(QTest::qWaitForWindowActive(&receiver, 5000));
  EventQueue events;
  AppUtilUnix appUtil(&events);
  const auto language = appUtil.getCurrentLanguageCode();
  class TracedState : public OSXKeyState
  {
  public:
    using OSXKeyState::OSXKeyState;
    int repeated = 0;
    void fakeKey(const Keystroke &stroke) override
    {
      if (stroke.m_type == Keystroke::KeyType::Button && stroke.m_data.m_button.m_repeat)
        ++repeated;
      OSXKeyState::fakeKey(stroke);
    }
  } state(&events, appUtil.getKeyboardLayoutList(), false);
  state.updateKeyMap();
  state.updateKeyState();
  const auto cleanup = qScopeGuard([&] { state.fakeAllKeysUp(); });
  auto pump = [] {
    QEventLoop loop;
    QTimer::singleShot(1, &loop, &QEventLoop::quit);
    loop.exec();
  };
  QString expected;
  QElapsedTimer timer;
  timer.start();
  for (int round = 0; round < (scenario == 3 ? 3 : 100); ++round) {
    QVERIFY2(receiver.isActiveWindow(), "Receiver lost focus; stopping injection");
    if (scenario == 0) {
      const QString pattern = "azerty QWERTY 0123456789 !?.,+-*/() ";
      for (int i = 0; i < pattern.size(); ++i) {
        QVERIFY2(receiver.isActiveWindow(), "Receiver lost focus; stopping injection");
        const KeyButton button = 100 + (i % 3);
        state.fakeKeyDown(pattern[i].unicode(), 0, button, language);
        expected += pattern[i];
        // Keep the previous physical key down while the next one is pressed.
        if (i > 0)
          QVERIFY(state.fakeKeyUp(100 + ((i - 1) % 3)));
        pump();
      }
      QVERIFY(state.fakeKeyUp(100 + ((pattern.size() - 1) % 3)));
    } else if (scenario == 1) {
      const KeyID navigation[] = {kKeyKP_Insert, kKeyKP_End,   kKeyKP_Down, kKeyKP_PageDown, kKeyKP_Left,
                                  kKeyKP_Begin,  kKeyKP_Right, kKeyKP_Home, kKeyKP_Up,       kKeyKP_PageUp};
      // Ten distinct physical keys held together; release them in a different order.
      for (int i = 0; i < 10; ++i) {
        state.fakeKeyDown(keypadDigits(navigation[i]), 0, 80 + i, language);
        expected += QChar('0' + i);
        pump();
      }
      for (int i : {2, 7, 1, 9}) {
        QVERIFY(state.fakeKeyRepeat(keypadDigits(navigation[i]), 0, 1, 80 + i, language));
        expected += QChar('0' + i);
        pump();
      }
      for (int i : {5, 0, 9, 1, 8, 2, 7, 3, 6, 4})
        QVERIFY(state.fakeKeyUp(80 + i));
    } else if (scenario == 3) {
      state.fakeKeyDown('x', 0, 53, language);
      expected += 'x';
      QTest::qWait(600);
      QVERIFY(receiver.isActiveWindow());
      for (int i = 0; i < 20; ++i) {
        QVERIFY(state.fakeKeyRepeat('x', 0, 1, 53, language));
        expected += 'x';
        QTest::qWait(20);
      }
      QVERIFY(state.fakeKeyUp(53));
    } else if (scenario == 4) {
      state.fakeKeyDown(kKeySuper_L, KeyModifierSuper, 133, language);
      state.fakeKeyDown('a', KeyModifierSuper, 38, language);
      QVERIFY(state.fakeKeyUp(38));
      QVERIFY(state.fakeKeyUp(133));
      pump();
      state.fakeKeyDown('x', 0, 53, language);
      QVERIFY(state.fakeKeyUp(53));
      expected = "x";
      pump();
    } else {
      state.fakeKeyDown(kKeyShift_L, KeyModifierShift, 50, language);
      state.fakeKeyDown('A', KeyModifierShift, 38, language);
      expected += 'A';
      QVERIFY(state.fakeKeyUp(50));
      QVERIFY(state.fakeKeyUp(38));
      state.fakeKeyDown('b', 0, 56, language);
      expected += 'b';
      QVERIFY(state.fakeKeyUp(56));
      pump();
    }
  }
  QTest::qWait(500);
  qInfo(
      "mixed scenario=%d expected=%lld actual=%lld elapsed=%lld ms", scenario, expected.size(), receiver.text().size(),
      timer.elapsed()
  );
  qInfo(
      "injected repeats=%d received presses=%d repeats=%d empty=%d", state.repeated, receiver.presses, receiver.repeats,
      receiver.empty
  );
  QCOMPARE(receiver.text(), expected);
  state.fakeAllKeysUp();
  QTest::qWait(100);
  const auto flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
  QVERIFY(
      !(flags & (kCGEventFlagMaskShift | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand))
  );
}

QTEST_MAIN(OSXKeyStateTests)
