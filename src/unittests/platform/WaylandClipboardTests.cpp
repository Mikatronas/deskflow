// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WaylandClipboardTests.h"
#include "platform/PortalClipboard.h"
#include "platform/WaylandClipboard.h"
#include <QBuffer>
#include <QImage>
#include <QScopeGuard>
using deskflow::WaylandClipboard;
using Format = IClipboard::Format;

void WaylandClipboardTests::roundTrip()
{
  if (!qEnvironmentVariableIsSet("DESKFLOW_AUDIT_CLIPBOARD"))
    QSKIP("Opt-in: temporarily replaces the desktop clipboard and restores its supported formats");
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Info);
  WaylandClipboard first, second;
  QTRY_VERIFY_WITH_TIMEOUT(first.ready() && second.ready(), 3000);
  QTest::qWait(500);
  auto original = first.take();
  QVERIFY2(original.has_value(), "Cannot safely test without a restorable clipboard snapshot");
  const auto restore = qScopeGuard([&] {
    first.publish(*original);
    QTest::qWait(500);
  });
  second.take();
  const WaylandClipboard::Snapshot expected{
      {Format::Text, "Deskflow native clipboard ✓\nsecond line"}, {Format::HTML, "<b>Deskflow</b>"}
  };
  first.publish(expected);
  std::optional<WaylandClipboard::Snapshot> received;
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = second.take()).has_value()), 3000);
  QVERIFY(*received == expected);
  QVERIFY(!first.take().has_value()); // own selection never echoes
  received.reset();
  second.publish({{Format::Text, ""}});
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = first.take()).has_value()), 3000);
  QVERIFY(received->contains(Format::Text));
  QVERIFY(received->at(Format::Text).empty());
  QImage image(32, 32, QImage::Format_RGB32);
  image.fill(Qt::red);
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  QVERIFY(image.save(&buffer, "PNG"));
  const auto dib = deskflow::PortalClipboard::decodeFormat(Format::Bitmap, png);
  QVERIFY(!dib.isEmpty());
  received.reset();
  second.publish({{Format::Bitmap, dib.toStdString()}});
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = first.take()).has_value()), 3000);
  QVERIFY(received->contains(Format::Bitmap));
  const auto roundTrip =
      deskflow::PortalClipboard::encodeFormat(Format::Bitmap, QByteArray::fromStdString(received->at(Format::Bitmap)));
  QCOMPARE(QImage::fromData(roundTrip), image);
  received.reset();
  const std::string large(1024 * 1024, 'x');
  second.publish({{Format::Text, large}});
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = first.take()).has_value()), 3000);
  QVERIFY(received->at(Format::Text) == large);
  received.reset();
  second.publish({});
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = first.take()).has_value()), 3000);
  QVERIFY(received->empty());
  first.setLimit(1024);
  second.publish({{Format::Text, std::string(2048, 'x')}});
  QTest::qWait(300);
  QVERIFY(!first.take().has_value()); // oversize data is never partially published
  received.reset();
  second.publish({{Format::Text, "recovered"}});
  QTRY_VERIFY_WITH_TIMEOUT((received.has_value() || (received = first.take()).has_value()), 3000);
  QCOMPARE(received->at(Format::Text), std::string("recovered"));
}
QTEST_MAIN(WaylandClipboardTests)
