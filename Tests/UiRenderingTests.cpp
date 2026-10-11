#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineF>
#include <QListWidget>
#include <QRawFont>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QTranslator>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QThread>
#include <QUrl>
#include <QTimer>
#include <QPainter>

#include "Source/Core/QuickConnect.h"
#include "Source/Core/Settings.h"
#include "Source/Core/SettingsRepository.h"
#include "Source/Gui/Glyph.h"
#include "Source/Gui/MainWindow.h"
#include "Source/Gui/SettingsWindow.h"
#include "Source/Gui/Theme.h"
#include "Source/Gui/TrayIcon.h"
#include "Source/Gui/UpdateWindow.h"

class UpdateUrlRecorder : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
    UpdateUrlRecorder()
    {
        QDesktopServices::setUrlHandler("https", this, "Open");
    }
    ~UpdateUrlRecorder() override
    {
        QDesktopServices::unsetUrlHandler("https");
    }
public Q_SLOTS:
    void Open(const QUrl &url)
    {
        urls.push_back(url);
    }
};

namespace {
void VerifySmoothPopupCorners(QWidget *popup, const QString &fileName)
{
    const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation/");
    QVERIFY(QDir{}.mkpath(outputDir));
    const auto image = popup->grab().toImage();
    QVERIFY(image.save(outputDir + fileName));
    QVERIFY(image.hasAlphaChannel());
    const int cornerSize = qRound(10 * image.devicePixelRatio());
    // Every corner must have a transparent exterior and intermediate alpha along the curve.
    // An opaque native background or a binary region mask fails these pixel checks.
    for (const bool right : {false, true}) {
        for (const bool bottom : {false, true}) {
            const auto pixelAt = [&](int x, int y) {
                return image.pixel(
                    right ? image.width() - 1 - x : x, bottom ? image.height() - 1 - y : y);
            };
            QCOMPARE(qAlpha(pixelAt(0, 0)), 0);
            QCOMPARE(qAlpha(pixelAt(cornerSize, cornerSize)), 255);
            int blendedPixels = 0;
            for (int y = 0; y < cornerSize; ++y) {
                for (int x = 0; x < cornerSize; ++x) {
                    const int alpha = qAlpha(pixelAt(x, y));
                    blendedPixels += alpha > 0 && alpha < 255;
                }
            }
            QVERIFY(blendedPixels > 0);
        }
    }
}

Core::Update::ReleaseInfo UpdateFixture()
{
    Core::Update::ReleaseInfo info;
    info.version = QVersionNumber{2, 12, 345};
    info.url = "https://example.invalid/release";
    info.isPreRelease = true;
    info.fileName = "simulated.exe";
    info.downloadUrl = "https://example.invalid/simulated.exe";
    info.fileSize = 4000000000u;
    info.sha256 = QString(64, QChar{'0'});
    info.changeLog =
        "Improved device connections.\nFixed battery status updates.\n<b>Plain text, not HTML</b>";
    return info;
}

struct SimulatedDownload {
    std::atomic<int> stage{0};
    std::atomic<int> calls{0};

    bool Run(const Core::Update::ReleaseInfo &, const Core::Update::FnProgress &progress)
    {
        ++calls;
        for (;;) {
            const auto current = stage.load();
            if (current >= 4) {
                return current == 4;
            }
            const size_t total = current == 0 ? 0 : 4000000000u;
            const size_t downloaded = current == 0 ? 0 : current == 1 ? 3000000000u : total;
            if (!progress(downloaded, total)) {
                return false;
            }
            QThread::msleep(10);
        }
    }

    Gui::UpdateWindow::DownloadFunction Function()
    {
        return [this](const auto &info, const auto &progress) { return Run(info, progress); };
    }
};

// Puts the theme back to the mode it had, even when a QVERIFY leaves the test early.
class ThemeModeGuard
{
public:
    ThemeModeGuard() : _mode{Gui::Theme::Manager::Instance().CurrentMode()} {}

    ~ThemeModeGuard()
    {
        Gui::Theme::Manager::Instance().SetMode(_mode);
    }

    ThemeModeGuard(const ThemeModeGuard &) = delete;
    ThemeModeGuard &operator=(const ThemeModeGuard &) = delete;

private:
    Gui::Theme::Mode _mode;
};

// Keeps the settings of the test run away from the persistent store of the user: every change a
// settings window writes lands in memory and is dropped with the scope.
class MemorySettingsScope
{
public:
    MemorySettingsScope()
    {
        Core::Settings::SetRepository(std::make_unique<Core::Settings::MemoryRepository>());
    }

    ~MemorySettingsScope()
    {
        Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
    }

    MemorySettingsScope(const MemorySettingsScope &) = delete;
    MemorySettingsScope &operator=(const MemorySettingsScope &) = delete;
};

// Records the stylesheet parser warnings that Qt logs instead of failing. The previous handler
// keeps receiving every other message, so the test log stays complete.
class StyleSheetWarningRecorder
{
public:
    StyleSheetWarningRecorder()
    {
        Warnings().clear();
        PreviousHandler() = qInstallMessageHandler(&Handle);
    }

    ~StyleSheetWarningRecorder()
    {
        qInstallMessageHandler(PreviousHandler());
    }

    StyleSheetWarningRecorder(const StyleSheetWarningRecorder &) = delete;
    StyleSheetWarningRecorder &operator=(const StyleSheetWarningRecorder &) = delete;

    const QStringList &Recorded() const
    {
        return Warnings();
    }

private:
    static QStringList &Warnings()
    {
        static QStringList warnings;
        return warnings;
    }

    static QtMessageHandler &PreviousHandler()
    {
        static QtMessageHandler handler = nullptr;
        return handler;
    }

    static void Handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        if (message.contains(QStringLiteral("Could not parse"))) {
            Warnings().push_back(message);
        }
        else if (PreviousHandler() != nullptr) {
            PreviousHandler()(type, context, message);
        }
    }
};

QString CssClass(const QObject *object)
{
    return object->property("cssClass").toString();
}

// WCAG 2.x contrast ratio between two opaque colours, from 1 to 21.
qreal ContrastRatio(const QColor &a, const QColor &b)
{
    const auto linear = [](qreal channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&](const QColor &color) {
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
               0.0722 * linear(color.blueF());
    };
    const auto first = luminance(a);
    const auto second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

bool HasVisiblePixel(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return true;
            }
        }
    }
    return false;
}

Core::AirPods::State AirPods5State()
{
    Core::AirPods::State state;
    state.model = Core::AirPods::Model::AirPods_5;
    state.displayName = QStringLiteral("AirPods 5");
    state.pods.left.battery = 80;
    state.pods.right.battery = 70;
    state.caseBox.battery = 50;
    return state;
}
} // namespace

class UiRenderingTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void MainWindowRendersAirPods5()
    {
        Core::AirPods::State state;
        state.model = Core::AirPods::Model::AirPods_5;
        state.displayName = QStringLiteral("AirPods 5");
        state.pods.left.battery = 80;
        state.pods.right.battery = 70;
        state.caseBox.battery = 50;

        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();
        const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation");
        QVERIFY(QDir{}.mkpath(outputDir));
        const auto scale = qEnvironmentVariable("QT_SCALE_FACTOR", "system");

        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            theme.SetMode(mode);
            Gui::MainWindow window;
            window.UpdateState(state);
            window.Show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));

            auto *label = window.findChild<QLabel *>("deviceLabel");
            auto *animation = window.findChild<Gui::Widget::AnimationView *>();
            QVERIFY(label != nullptr);
            QVERIFY(animation != nullptr);
            QCOMPARE(label->text(), state.displayName);
            QTRY_VERIFY(animation->isVisible());
            QCOMPARE(animation->width(), animation->height() * 2);

            const auto animationImage = animation->grab().toImage();
            int visiblePixels = 0;
            for (int y = 0; y < animationImage.height(); y += 2) {
                for (int x = 0; x < animationImage.width(); x += 2) {
                    visiblePixels += qAlpha(animationImage.pixel(x, y)) != 0;
                }
            }
            QVERIFY(visiblePixels > 100);

            const auto suffix =
                QString{"airpods-5-%1-scale%2.png"}.arg(theme.IsDark() ? "dark" : "light", scale);
            QVERIFY(window.grab().save(outputDir + '/' + suffix));
            window.close();
        }
        theme.SetMode(originalMode);
    }

    void MainWindowElidesLongDeviceNames()
    {
        Gui::MainWindow window;
        auto *label = window.findChild<QLabel *>("deviceLabel");
        QVERIFY(label != nullptr);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(!label->wordWrap());

        Core::AirPods::State state;
        state.model = Core::AirPods::Model::AirPods_4;
        state.displayName = QStringLiteral("AirPods 4");
        window.UpdateState(state);
        QCOMPARE(label->text(), state.displayName);
        QCOMPARE(label->font().pointSize(), 18);
        QCOMPARE(label->toolTip(), QString{});
        QCOMPARE(label->accessibleName(), state.displayName);

        state.displayName =
            QStringLiteral("結城さくな專用的超級無敵特別限定版 AirPods Pro 第二世代");
        window.UpdateState(state);
        QCOMPARE(label->font().pointSize(), 12);
        QVERIFY(label->text() != state.displayName);
        QVERIFY(label->text().contains(QChar{0x2026}));
        QVERIFY(!label->text().startsWith(QChar{0x2026}));
        QVERIFY(!label->text().endsWith(QChar{0x2026}));
        QVERIFY(
            label->fontMetrics().horizontalAdvance(label->text()) <= label->contentsRect().width());
        QCOMPARE(label->toolTip(), state.displayName);
        QCOMPARE(label->accessibleName(), state.displayName);

        const auto scale = qEnvironmentVariable("QT_SCALE_FACTOR", "system");
        VerifySmoothPopupCorners(
            &window, QString{"main-window-long-device-name-scale%1.png"}.arg(scale));
    }

    void UpdateTextHasAntialiasedEdges_data()
    {
        QTest::addColumn<QString>("localeName");
        QTest::newRow("English") << "en_US";
        QTest::newRow("Traditional Chinese") << "zh_TW";
    }

    void UpdateTextHasAntialiasedEdges()
    {
        QFETCH(QString, localeName);
        Gui::Theme::ApplyApplicationTypography(QLocale{localeName});
        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();
        Gui::UpdateWindow window{UpdateFixture()};
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            theme.SetMode(mode);
            for (const auto *name : {"promptTitle", "rowSubtitle", "updateButton", "releaseNotes"})
            {
                const auto *widget = window.findChild<QWidget *>(name);
                QVERIFY(widget);
                const auto ratio = window.devicePixelRatioF();
                QPixmap textImage{QSize{400, 60} * ratio};
                textImage.setDevicePixelRatio(ratio);
                const bool dark = mode == Gui::Theme::Mode::Dark;
                textImage.fill(dark ? Qt::black : Qt::white);
                {
                    QPainter painter{&textImage};
                    painter.setFont(widget->font());
                    painter.setPen(dark ? Qt::white : Qt::black);
                    painter.drawText(QPoint{8, 30}, QStringLiteral("AirPodsDesktop 更新版本 2.12"));
                }
                const auto pixels = textImage.toImage();
                int intermediatePixels = 0;
                for (int y = 0; y < pixels.height(); ++y) {
                    for (int x = 0; x < pixels.width(); ++x) {
                        const auto pixel = pixels.pixel(x, y);
                        if (qRed(pixel) > 0 && qRed(pixel) < 255) {
                            ++intermediatePixels;
                        }
                    }
                }
                // Check smooth raster edges while allowing the system's ClearType rendering.
                QVERIFY2(intermediatePixels > 0, name);
            }
        }
        Gui::Theme::ApplyApplicationTypography(QLocale{"en_US"});
        theme.SetMode(originalMode);
    }

    void UpdatePromptActions()
    {
        UpdateUrlRecorder recorder;
        for (const auto *name : {"skipButton", "laterButton", "escape", "close"}) {
            Gui::UpdateWindow window{UpdateFixture()};
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *notes = window.findChild<QPlainTextEdit *>("releaseNotes");
            QVERIFY(notes->isReadOnly());
            QCOMPARE(notes->toPlainText(), UpdateFixture().changeLog);
            QTest::mouseClick(window.findChild<QPushButton *>("viewButton"), Qt::LeftButton);
            QVERIFY(window.isVisible());
            QCOMPARE(recorder.urls.last(), QUrl{UpdateFixture().url});
            if (QString{name} == "escape") {
                QTest::keyClick(&window, Qt::Key_Escape);
            }
            else if (QString{name} == "close") {
                window.close();
            }
            else {
                QTest::mouseClick(window.findChild<QPushButton *>(name), Qt::LeftButton);
            }
            const auto expected =
                QString{name} == "updateButton" ? Gui::UpdateWindow::Action::Update
                : QString{name} == "skipButton" ? Gui::UpdateWindow::Action::Skip
                                                : Gui::UpdateWindow::Action::Later;
            QCOMPARE(window.SelectedAction(), expected);
            QVERIFY(!window.isVisible());
        }
    }

    void UnsupportedUpdateOpensReleaseWithoutDownloading()
    {
        UpdateUrlRecorder recorder;
        auto info = UpdateFixture();
        info.sha256.clear();
        SimulatedDownload download;
        Gui::UpdateWindow window{info, nullptr, download.Function()};
        window.show();
        window.findChild<QPushButton *>("updateButton")->click();
        QCOMPARE(download.calls.load(), 0);
        QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::ManualDownload);
        QCOMPARE(window.SelectedAction(), Gui::UpdateWindow::Action::Update);
        QCOMPARE(recorder.urls, QList<QUrl>{QUrl{info.url}});
        QVERIFY(!window.isVisible());
    }

    void DownloadProgressAndCompletion()
    {
        SimulatedDownload download;
        Gui::UpdateWindow window{UpdateFixture(), nullptr, download.Function()};
        QCOMPARE(download.calls.load(), 0);
        QSignalSpy finished{&window, &QDialog::finished};
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto nativeWindow = window.winId();
        auto *notes = window.findChild<QPlainTextEdit *>("releaseNotes");
        const auto notesPosition = notes->mapTo(&window, QPoint{});
        const auto windowPosition = window.pos();
        QTest::mouseClick(window.findChild<QPushButton *>("updateButton"), Qt::LeftButton);
        window.StartDownload();
        QCOMPARE(window.winId(), nativeWindow);
        QCOMPARE(window.SelectedAction(), Gui::UpdateWindow::Action::Update);
        QCOMPARE(notes->mapTo(&window, QPoint{}), notesPosition);
        QCOMPARE(window.pos(), windowPosition);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *bar = window.findChild<QProgressBar *>("progressBar");
        QTRY_COMPARE(download.calls.load(), 1);
        QCOMPARE(bar->maximum(), 0);
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(!window.close());
        QVERIFY(window.isVisible());
        download.stage = 1;
        QTRY_COMPARE(bar->value(), 750);
        QVERIFY(window.findChild<QLabel *>("progressDetails")->text().contains("75%"));
        window.UpdateProgressSafely(quint64{1} << 32, quint64{1} << 33);
        QCOMPARE(bar->value(), 500);
        download.stage = 0;
        QTRY_COMPARE(bar->maximum(), 0);
        const auto downloadingHeight = window.height();
        download.stage = 2;
        QTRY_COMPARE(bar->value(), 1000);
        QVERIFY(!window.findChild<QPushButton *>("pushButtonDownloadManually")->isEnabled());
        QVERIFY(window.findChild<QLabel *>("hint")->isHidden());
        QVERIFY(window.height() < downloadingHeight);
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(!window.close());
        QCOMPARE(bar->accessibleName(), QString{"Preparing to install..."});
        QCOMPARE(finished.count(), 0);
        QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::KeepRunning);
        download.stage = 4;
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::InstallerStarted);
        QVERIFY(!window.isVisible());
    }

    void DownloadFailureAndManualActions()
    {
        UpdateUrlRecorder recorder;
        for (const auto *action : {"closeButton", "escape", "close"}) {
            Gui::UpdateWindow window{
                UpdateFixture(), nullptr, [](const auto &, const auto &) { return false; }};
            QSignalSpy finished{&window, &QDialog::finished};
            window.StartDownload();
            window.show();
            auto *close = window.findChild<QPushButton *>("closeButton");
            QTRY_VERIFY(close->isVisible());
            QVERIFY(window.windowFlags().testFlag(Qt::WindowCloseButtonHint));
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&window));
            QCOMPARE(QApplication::focusWidget(), close);
            const auto focusedSize = close->size();
            window.findChild<QPushButton *>("viewButton")->setFocus();
            QCOMPARE(close->size(), focusedSize);
            close->setFocus();
            QCOMPARE(finished.count(), 0);
            QVERIFY(!window.findChild<QProgressBar *>("progressBar")->isVisible());
            const auto failureText = window.findChild<QLabel *>("failureDescription")->text();
            window.UpdateProgressSafely(100, 100);
            QCOMPARE(window.findChild<QLabel *>("failureDescription")->text(), failureText);
            const auto urlCount = recorder.urls.size();
            QTest::mouseClick(
                window.findChild<QPushButton *>("failedManualButton"), Qt::LeftButton);
            QCOMPARE(recorder.urls.size(), urlCount + 1);
            QVERIFY(window.isVisible());
            QCOMPARE(finished.count(), 0);
            QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::KeepRunning);
            if (QString{action} == "escape") {
                QTest::keyClick(&window, Qt::Key_Escape);
            }
            else if (QString{action} == "close") {
                window.close();
            }
            else {
                QTest::mouseClick(close, Qt::LeftButton);
            }
            QCOMPARE(finished.count(), 1);
            QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::KeepRunning);
            QCOMPARE(window.SelectedAction(), Gui::UpdateWindow::Action::Update);
        }
        SimulatedDownload download;
        Gui::UpdateWindow active{UpdateFixture(), nullptr, download.Function()};
        active.StartDownload();
        active.show();
        QVERIFY(QTest::qWaitForWindowExposed(&active));
        QTest::mouseClick(
            active.findChild<QPushButton *>("pushButtonDownloadManually"), Qt::LeftButton);
        QCOMPARE(active.Result(), Gui::UpdateWindow::Outcome::ManualDownload);
        QVERIFY(!active.isVisible());
    }

    void FailedDownloadStaysInModalLoop()
    {
        Gui::UpdateWindow window{
            UpdateFixture(), nullptr, [](const auto &, const auto &) { return false; }};
        bool failureWasVisible = false;
        bool userClosed = false;
        QTimer closeWhenFailed;
        closeWhenFailed.setInterval(10);
        connect(&closeWhenFailed, &QTimer::timeout, &window, [&] {
            auto *button = window.findChild<QPushButton *>("closeButton");
            if (button->isVisible()) {
                failureWasVisible = window.isVisible();
                userClosed = true;
                button->click();
            }
        });
        QTimer::singleShot(3000, &window, &QDialog::accept);
        closeWhenFailed.start();
        window.StartDownload();
        window.exec();
        QVERIFY(failureWasVisible);
        QVERIFY(userClosed);
        QCOMPARE(window.Result(), Gui::UpdateWindow::Outcome::KeepRunning);
    }

    void UpdateWindowsFollowLanguageAndThemeChanges()
    {
        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();
        Gui::UpdateWindow prompt{UpdateFixture()};
        Gui::UpdateWindow failure{
            UpdateFixture(), nullptr, [](const auto &, const auto &) { return false; }};
        failure.StartDownload();
        failure.show();
        QTRY_VERIFY(failure.findChild<QPushButton *>("closeButton")->isVisible());
        const auto englishTitle = failure.findChild<QLabel *>("title")->text();
        QTranslator translator;
        QVERIFY(translator.load(
            QLocale{"zh_TW"}, "apd", "_",
            QCoreApplication::applicationDirPath() + "/translations"));
        QVERIFY(QCoreApplication::installTranslator(&translator));
        QCoreApplication::processEvents();
        QCOMPARE(
            failure.findChild<QLabel *>("title")->text(),
            translator.translate("Gui::UpdateWindow", "Unable to update"));
        QCOMPARE(
            prompt.findChild<QPushButton *>("updateButton")->text(),
            translator.translate("UpdateWindow", "Update now"));
        QCOMPARE(
            prompt.findChild<QPlainTextEdit *>("releaseNotes")->toPlainText(),
            UpdateFixture().changeLog);
        theme.SetMode(Gui::Theme::Mode::Dark);
        QCOMPARE(failure.palette().color(QPalette::Window), theme.Colors().windowBackground);
        theme.SetMode(Gui::Theme::Mode::Light);
        QCOMPARE(failure.palette().color(QPalette::Window), theme.Colors().windowBackground);
        QCoreApplication::removeTranslator(&translator);
        QCoreApplication::processEvents();
        QCOMPARE(failure.findChild<QLabel *>("title")->text(), englishTitle);
        QVERIFY(failure.isVisible());
        failure.close();
        theme.SetMode(originalMode);
    }

    void UpdateWindowsRendering_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("localeName");
        for (const auto *locale : {"en_US", "zh_TW"}) {
            QTest::newRow(qPrintable(QString{locale} + "-light")) << false << locale;
            QTest::newRow(qPrintable(QString{locale} + "-dark")) << true << locale;
        }
    }

    void UpdateWindowsRendering()
    {
        QFETCH(bool, dark);
        QFETCH(QString, localeName);
        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();
        QTranslator translator;
        if (localeName == "zh_TW") {
            QVERIFY(translator.load(
                QLocale{localeName}, "apd", "_",
                QCoreApplication::applicationDirPath() + "/translations"));
            QVERIFY(QCoreApplication::installTranslator(&translator));
        }
        Gui::Theme::ApplyApplicationTypography(QLocale{localeName});
        theme.SetMode(dark ? Gui::Theme::Mode::Dark : Gui::Theme::Mode::Light);
        const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation");
        QDir{}.mkpath(outputDir);
        const auto suffix = QString{"-%1-%2-scale%3.png"}.arg(
            localeName, dark ? "dark" : "light", qEnvironmentVariable("QT_SCALE_FACTOR", "1"));
        const auto verifyLayout = [](QWidget &window) {
            if (window.width() != 560) {
                return false;
            }
            for (auto *button : window.findChildren<QPushButton *>()) {
                if (button->isVisible() &&
                    (!window.rect().contains(
                         QRect{button->mapTo(&window, QPoint{}), button->size()}) ||
                     button->height() != 28 ||
                     button->width() < button->fontMetrics().horizontalAdvance(button->text()) +
                                           (button->isFlat() ? 14 : 32)))
                {
                    qWarning() << button->objectName() << button->size() << button->text()
                               << button->fontMetrics().horizontalAdvance(button->text());
                    return false;
                }
            }
            auto *status = window.findChild<QLabel *>("status");
            auto *details = window.findChild<QLabel *>("progressDetails");
            if (status->isVisible() &&
                (status->fontMetrics().horizontalAdvance(status->text()) > status->width() ||
                 details->fontMetrics().horizontalAdvance(details->text()) > details->width() ||
                 status->geometry().intersects(details->geometry())))
            {
                qWarning() << status->geometry() << details->geometry();
                return false;
            }
            return true;
        };
        auto stableInfo = UpdateFixture();
        stableInfo.isPreRelease = false;
        Gui::UpdateWindow stable{stableInfo};
        stable.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stable));
        QVERIFY(stable.grab().save(outputDir + "/update-stable" + suffix));
        QVERIFY(verifyLayout(stable));
        stable.close();
        Gui::UpdateWindow prompt{UpdateFixture()};
        prompt.show();
        QVERIFY(QTest::qWaitForWindowExposed(&prompt));
        QVERIFY(verifyLayout(prompt));
        QVERIFY(prompt.grab().save(outputDir + "/update-prompt" + suffix));
        auto *notes = prompt.findChild<QPlainTextEdit *>("releaseNotes");
        prompt.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&prompt));
        notes->setFocus();
        QTest::keyClick(notes, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), prompt.findChild<QPushButton *>("viewButton"));
        prompt.close();
        auto longInfo = UpdateFixture();
        longInfo.version = QVersionNumber{20260904, 12345678, 2147483647};
        longInfo.changeLog = QString{"A long line without spaces: "} + QString(600, QChar{'X'}) +
                             QString{"\nRelease note\n"}.repeated(100);
        Gui::UpdateWindow longPrompt{longInfo};
        longPrompt.show();
        QVERIFY(QTest::qWaitForWindowExposed(&longPrompt));
        QVERIFY(verifyLayout(longPrompt));
        QTRY_VERIFY(
            longPrompt.findChild<QPlainTextEdit *>("releaseNotes")->verticalScrollBar()->maximum() >
            0);
        QVERIFY(longPrompt.grab().save(outputDir + "/update-long" + suffix));
        longPrompt.close();
        auto emptyInfo = UpdateFixture();
        emptyInfo.changeLog.clear();
        emptyInfo.isPreRelease = false;
        Gui::UpdateWindow emptyPrompt{emptyInfo};
        emptyPrompt.show();
        QVERIFY(QTest::qWaitForWindowExposed(&emptyPrompt));
        QVERIFY(
            emptyPrompt.findChild<QPlainTextEdit *>("releaseNotes")->isHidden() ||
            !emptyPrompt.findChild<QPlainTextEdit *>("releaseNotes")->isVisible());
        QVERIFY(emptyPrompt.findChild<QWidget *>("noChangeLogRow")->isVisible());
        QVERIFY(verifyLayout(emptyPrompt));
        QVERIFY(emptyPrompt.grab().save(outputDir + "/update-empty" + suffix));
        emptyPrompt.close();
        SimulatedDownload download;
        download.stage = 0;
        Gui::UpdateWindow progress{UpdateFixture(), nullptr, download.Function()};
        progress.show();
        QVERIFY(QTest::qWaitForWindowExposed(&progress));
        const auto notesPosition =
            progress.findChild<QPlainTextEdit *>("releaseNotes")->mapTo(&progress, QPoint{});
        const auto nativeWindow = progress.winId();
        progress.findChild<QPushButton *>("updateButton")->click();
        QCOMPARE(progress.winId(), nativeWindow);
        QCOMPARE(
            progress.findChild<QPlainTextEdit *>("releaseNotes")->mapTo(&progress, QPoint{}),
            notesPosition);
        QTRY_COMPARE(progress.findChild<QProgressBar *>("progressBar")->maximum(), 0);
        QVERIFY(verifyLayout(progress));
        QVERIFY(progress.grab().save(outputDir + "/update-waiting" + suffix));
        download.stage = 1;
        QTRY_COMPARE(progress.findChild<QProgressBar *>("progressBar")->value(), 750);
        QVERIFY(verifyLayout(progress));
        QVERIFY(progress.grab().save(outputDir + "/update-download" + suffix));
        download.stage = 2;
        QTRY_COMPARE(progress.findChild<QProgressBar *>("progressBar")->value(), 1000);
        QVERIFY(verifyLayout(progress));
        QVERIFY(progress.grab().save(outputDir + "/update-preparing" + suffix));
        download.stage = 5;
        QTRY_VERIFY(progress.findChild<QPushButton *>("closeButton")->isVisible());
        QVERIFY(verifyLayout(progress));
        QVERIFY(progress.grab().save(outputDir + "/update-failure" + suffix));
        progress.close();
        theme.SetMode(originalMode);
        QCoreApplication::removeTranslator(&translator);
        Gui::Theme::ApplyApplicationTypography(QLocale{"en_US"});
    }

    void BundledTypographyLoadsInterFamilies()
    {
        QVERIFY(Gui::Theme::LoadBundledFonts());

        const auto families = QFontDatabase{}.families();
        QVERIFY(families.contains("Inter"));
        QVERIFY(families.contains("Inter Display"));
        QVERIFY(families.contains("Noto Sans TC"));

        QFont traditionalChineseFont{"Noto Sans TC"};
        const auto traditionalChineseSample = QString::fromUtf8(
            "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87\xE8\xA8\xAD\xE5\xAE\x9A");
        const auto glyphs = QRawFont::fromFont(traditionalChineseFont)
                                .glyphIndexesForString(traditionalChineseSample);
        QVERIFY(!glyphs.isEmpty());
        QVERIFY(
            std::all_of(glyphs.cbegin(), glyphs.cend(), [](quint32 glyph) { return glyph != 0; }));

        const auto applicationFont = Gui::Theme::ApplicationFont(QLocale{"zh_TW"});
        QCOMPARE(applicationFont.families().first(), QStringLiteral("Inter"));
        QCOMPARE(applicationFont.families().at(1), QStringLiteral("Noto Sans TC"));
        QCOMPARE(
            Gui::Theme::DisplayFontFamilies(QLocale{"zh_TW"}).first(),
            QStringLiteral("Inter Display"));

        struct LocaleFontCase {
            const char *locale;
            const char *bodyFallback;
            const char *displayFallback;
        };
        constexpr LocaleFontCase cases[]{
            {"zh_CN", "Microsoft YaHei UI", "Microsoft YaHei UI"},
            {"zh_TW", "Noto Sans TC", "Noto Sans TC"},
            {"ja_JP", "Yu Gothic UI", "Yu Gothic UI"},
            {"ko_KR", "Malgun Gothic", "Malgun Gothic"},
            {"de_DE", "Segoe UI Variable Text", "Inter"},
        };
        for (const auto &fontCase : cases) {
            QCOMPARE(
                Gui::Theme::ApplicationFont(QLocale{fontCase.locale}).families().at(1),
                QString::fromLatin1(fontCase.bodyFallback));
            QCOMPARE(
                Gui::Theme::DisplayFontFamilies(QLocale{fontCase.locale}).at(1),
                QString::fromLatin1(fontCase.displayFallback));
        }
    }

    void SettingsWindowSupportsKeyboardAndCompactLayout()
    {
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};

        Gui::Theme::ApplyApplicationTypography(QLocale{"zh_TW"});
        Gui::SettingsWindow window{[] { return 0; }, quickConnect};
        window.resize(640, 420);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *languages = window.findChild<QComboBox *>("cbLanguages");
        QVERIFY(languages != nullptr);
        QCOMPARE(languages->itemText(0), QStringLiteral("English"));

        auto *navigation = window.findChild<QListWidget *>("navList");
        QVERIFY(navigation != nullptr);
        QCOMPARE(navigation->frameShape(), QFrame::NoFrame);
        QCOMPARE(navigation->focusPolicy(), Qt::StrongFocus);
        QVERIFY(!qApp->styleSheet().contains(QStringLiteral("border-left")));
        // Qt only logs a parse warning for an unresolved @token, so check for one explicitly.
        QVERIFY(!qApp->styleSheet().contains(QLatin1Char('@')));
        navigation->setFocus();
        QCOMPARE(QApplication::focusWidget(), navigation);
        QCOMPARE(navigation->currentRow(), 0);
        QTest::keyClick(navigation, Qt::Key_Down);
        QCOMPARE(navigation->currentRow(), 1);

        auto *appearanceMode = window.findChild<QComboBox *>("cbAppearanceMode");
        QVERIFY(appearanceMode != nullptr);
        QCOMPARE(appearanceMode->count(), 3);
        QCOMPARE(appearanceMode->sizeAdjustPolicy(), QComboBox::AdjustToContents);
        QVERIFY(!appearanceMode->accessibleName().isEmpty());

        const auto appearanceTextFits = [appearanceMode] {
            QStyleOptionComboBox option;
            option.initFrom(appearanceMode);
            option.currentText = appearanceMode->currentText();
            const auto textRect = appearanceMode->style()->subControlRect(
                QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, appearanceMode);
            const QFontMetrics metrics{appearanceMode->font()};
            for (int i = 0; i < appearanceMode->count(); ++i) {
                if (metrics.horizontalAdvance(appearanceMode->itemText(i)) > textRect.width()) {
                    return false;
                }
            }
            return true;
        };
        QVERIFY(appearanceTextFits());

        for (const auto *name : {
                 "cbLowAudioLatency",
                 "cbAutoEarDetection",
                 "cbTrayQuickConnectEnabled",
             })
        {
            auto *checkBox = window.findChild<QCheckBox *>(name);
            QVERIFY(checkBox != nullptr);
            QVERIFY(!checkBox->accessibleName().isEmpty());
            QVERIFY(checkBox->minimumWidth() >= 32);
            QVERIFY(checkBox->minimumHeight() >= 32);
        }

        auto *scrollArea = window.findChild<QScrollArea *>("pagesScrollArea");
        QVERIFY(scrollArea != nullptr);
        QVERIFY(scrollArea->widgetResizable());
        QVERIFY(!scrollArea->horizontalScrollBar()->isVisible());

        for (auto *radioButton : window.findChildren<QRadioButton *>()) {
            if (!radioButton->isVisible())
                continue;

            const auto rightEdge =
                radioButton->mapTo(scrollArea->viewport(), radioButton->rect().topRight()).x();
            QVERIFY2(
                rightEdge < scrollArea->viewport()->width(),
                radioButton->objectName().toUtf8().constData());
        }

        auto *buttonBox = window.findChild<QDialogButtonBox *>("buttonBox");
        QVERIFY(buttonBox != nullptr);
        const auto buttonBottom = buttonBox->mapTo(&window, buttonBox->rect().bottomLeft()).y();
        QVERIFY(buttonBottom < window.height());

        QTranslator traditionalChinese;
        const auto translationsDir = QCoreApplication::applicationDirPath() + "/translations";
        QVERIFY(traditionalChinese.load(QLocale{"zh_TW"}, "apd", "_", translationsDir));
        QVERIFY(QCoreApplication::installTranslator(&traditionalChinese));
        QCoreApplication::processEvents();
        QCOMPARE(navigation->item(0)->text(), QString::fromUtf8("\xE4\xB8\x80\xE8\x88\xAC"));
        QCOMPARE(navigation->item(1)->text(), QString::fromUtf8("\xE5\xA4\x96\xE8\xA7\x80"));
        QTRY_COMPARE(
            buttonBox->button(QDialogButtonBox::RestoreDefaults)->text(),
            QString::fromUtf8("\xE9\x82\x84\xE5\x8E\x9F\xE9\xA0\x90\xE8\xA8\xAD\xE5\x80\xBC"));
        QTRY_COMPARE(
            buttonBox->button(QDialogButtonBox::Close)->text(),
            QString::fromUtf8("\xE9\x97\x9C\xE9\x96\x89"));
        QVERIFY(appearanceTextFits());

        const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation");
        QDir{}.mkpath(outputDir);
        QVERIFY(window.grab().save(outputDir + "/settings-current-theme.png"));

        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();

        theme.SetMode(Gui::Theme::Mode::Dark);
        QVERIFY(theme.IsDark());
        QCOMPARE(theme.Colors().windowBackground, QColor{"#1C1C1E"});
        QCOMPARE(theme.Colors().surface, QColor{"#2C2C2E"});
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(outputDir + "/settings-dark.png"));
        appearanceMode->showPopup();
        QTRY_VERIFY(QApplication::activePopupWidget() != nullptr);
        auto *comboPopup = QApplication::activePopupWidget();
        QCOMPARE(comboPopup->objectName(), QStringLiteral("apdComboPopup"));
        QVERIFY(appearanceMode->view()->viewport()->mapTo(comboPopup, QPoint{}).x() >= 4);
        QVERIFY(!appearanceMode->view()->verticalScrollBar()->isVisible());
        appearanceMode->view()->setCurrentIndex(appearanceMode->model()->index(0, 0));
        QVERIFY(appearanceMode->view()->selectionModel()->isSelected(
            appearanceMode->model()->index(0, 0)));
        QTest::qWait(100);
        VerifySmoothPopupCorners(comboPopup, "combo-popup-dark.png");
        appearanceMode->hidePopup();

        theme.SetMode(Gui::Theme::Mode::Light);
        QVERIFY(!theme.IsDark());
        QCOMPARE(theme.Colors().windowBackground, QColor{"#F5F5F7"});
        QCOMPARE(theme.Colors().surface, QColor{"#FFFFFF"});
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(outputDir + "/settings-light.png"));
        appearanceMode->showPopup();
        QTRY_VERIFY(QApplication::activePopupWidget() != nullptr);
        comboPopup = QApplication::activePopupWidget();
        QCOMPARE(comboPopup->objectName(), QStringLiteral("apdComboPopup"));
        QVERIFY(appearanceMode->view()->viewport()->mapTo(comboPopup, QPoint{}).x() >= 4);
        QVERIFY(!appearanceMode->view()->verticalScrollBar()->isVisible());
        appearanceMode->view()->setCurrentIndex(appearanceMode->model()->index(2, 0));
        QVERIFY(appearanceMode->view()->selectionModel()->isSelected(
            appearanceMode->model()->index(2, 0)));
        QTest::qWait(100);
        VerifySmoothPopupCorners(comboPopup, "combo-popup-light.png");
        appearanceMode->hidePopup();

        theme.SetMode(originalMode);
        QCoreApplication::removeTranslator(&traditionalChinese);
        Gui::Theme::ApplyApplicationTypography(QLocale{"en_US"});
    }

    void TrayMenuHasSmoothCorners()
    {
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};
        Gui::TrayIcon tray{[] { return 0; }, quickConnect};
        auto *menu = tray.findChild<QMenu *>();
        QVERIFY(menu != nullptr);
        const auto actions = menu->actions();
        const auto checkableAction =
            std::find_if(actions.cbegin(), actions.cend(), [](const QAction *action) {
                return action->isCheckable();
            });
        QVERIFY(checkableAction != actions.cend());
        (*checkableAction)->setChecked(true);
        auto &theme = Gui::Theme::Manager::Instance();
        const auto originalMode = theme.CurrentMode();
        QDir{}.mkpath(QStringLiteral(APD_BINARY_DIR "/UiValidation"));
        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            theme.SetMode(mode);
            tray.ShowContextMenu(QPoint{100, 100});
            QVERIFY(QTest::qWaitForWindowExposed(menu));
            for (const bool last : {false, true}) {
                if (last) {
                    menu->setActiveAction(menu->actions().last());
                }
                else {
                    QTest::keyClick(menu, Qt::Key_Down);
                }
                QVERIFY(menu->activeAction() != nullptr);
                VerifySmoothPopupCorners(
                    menu, QString{"tray-menu-%1-%2.png"}
                              .arg(theme.IsDark() ? "dark" : "light")
                              .arg(last ? "last" : "first"));
            }
            QTest::keyClick(menu, Qt::Key_Escape);
            QTRY_VERIFY(!menu->isVisible());
        }
        theme.SetMode(originalMode);
    }

    void SettingsDescriptionsFollowConsecutiveLanguageChanges()
    {
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};
        Gui::SettingsWindow window{[] { return 0; }, quickConnect};

        auto *description = window.findChild<QLabel *>("lbDescLowAudioLatency");
        QVERIFY(description != nullptr);

        constexpr auto sourceText =
            "Keeps the audio device awake while your AirPods are connected so short sounds start "
            "immediately. This may produce audible hiss and use more battery.";
        const auto translationsDir = QCoreApplication::applicationDirPath() + "/translations";

        QTranslator first;
        QVERIFY(first.load(QLocale{"zh_TW"}, "apd", "_", translationsDir));
        const auto firstDescription = first.translate("QObject", sourceText);
        QVERIFY(!firstDescription.isEmpty());
        Gui::Theme::ApplyApplicationTypography(QLocale{"zh_TW"});
        QVERIFY(QCoreApplication::installTranslator(&first));
        QCoreApplication::processEvents();
        QCOMPARE(description->text(), firstDescription);
        QCOMPARE(qApp->font().families().at(1), QStringLiteral("Noto Sans TC"));

        QTranslator second;
        QVERIFY(second.load(QLocale{"de_DE"}, "apd", "_", translationsDir));
        const auto secondDescription = second.translate("QObject", sourceText);
        QVERIFY(!secondDescription.isEmpty());
        QCoreApplication::removeTranslator(&first);
        Gui::Theme::ApplyApplicationTypography(QLocale{"de_DE"});
        QVERIFY(QCoreApplication::installTranslator(&second));
        QCoreApplication::processEvents();
        QCOMPARE(description->text(), secondDescription);
        QCOMPARE(qApp->font().families().at(1), QStringLiteral("Segoe UI Variable Text"));

        QCoreApplication::removeTranslator(&second);
        Gui::Theme::ApplyApplicationTypography(QLocale{"en_US"});
    }

    void StylesheetIsWellFormed()
    {
        ThemeModeGuard modeGuard;
        auto &theme = Gui::Theme::Manager::Instance();
        const QRegularExpression resourceUrl{R"re(url\((:/[^)]+)\))re"};

        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            // Installed before the mode change, so it also sees what the theme manager triggers
            // while it applies the sheet itself.
            StyleSheetWarningRecorder recorder;
            theme.SetMode(mode);

            const auto sheet = qApp->styleSheet();
            QVERIFY(!sheet.isEmpty());
            QVERIFY2(!sheet.contains(QLatin1Char('@')), "unresolved @token in the stylesheet");
            QVERIFY(!sheet.contains(QStringLiteral("border-left")));

            // An image that is missing from Resource.qrc is silently skipped by the style.
            int resourceCount = 0;
            for (auto matches = resourceUrl.globalMatch(sheet); matches.hasNext();) {
                const auto resource = matches.next().captured(1);
                QVERIFY2(QFile::exists(resource), qPrintable(resource));
                ++resourceCount;
            }
            QVERIFY(resourceCount > 0);

            // Applying a sheet drops the parsed rules, and polishing a widget parses them again.
            qApp->setStyleSheet(sheet);
            QPushButton probe;
            probe.ensurePolished();
            QVERIFY2(
                recorder.Recorded().isEmpty(),
                qPrintable(recorder.Recorded().join(QStringLiteral("\n"))));
        }
    }

    void MainWindowHeaderButtonsRequestSettings()
    {
        Gui::MainWindow window;
        window.Show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *gear = window.findChild<QAbstractButton *>("settingsButton");
        auto *close = window.findChild<QAbstractButton *>("closeButton");
        auto *label = window.findChild<QLabel *>("deviceLabel");
        QVERIFY(gear != nullptr);
        QVERIFY(close != nullptr);
        QVERIFY(label != nullptr);

        // Both buttons sit on the centres of the corner arcs of the 26px card, so they stay
        // concentric with the corners and clear of the device name.
        QCOMPARE(gear->geometry(), QRect(12, 12, 28, 28));
        QCOMPARE(close->geometry(), QRect(280, 12, 28, 28));
        QVERIFY(!gear->geometry().intersects(label->geometry()));
        QVERIFY(!close->geometry().intersects(label->geometry()));
        const auto offsetFrom = [](const QAbstractButton *button, const QPointF &arcCentre) {
            return QLineF{QRectF{button->geometry()}.center(), arcCentre}.length();
        };
        QVERIFY(offsetFrom(gear, QPointF{26, 26}) + 14 <= 26);
        QVERIFY(offsetFrom(close, QPointF{294, 26}) + 14 <= 26);

        for (const auto *button : {gear, close}) {
            QVERIFY(!button->toolTip().isEmpty());
            QVERIFY(!button->accessibleName().isEmpty());
        }

        QSignalSpy settingsRequested{&window, &Gui::MainWindow::SettingsRequested};
        QVERIFY(settingsRequested.isValid());

        QTest::mouseClick(gear, Qt::LeftButton, Qt::NoModifier, gear->rect().center());
        QCOMPARE(settingsRequested.count(), 1);

        // Only the circle reacts: a release outside of it cancels the click, and so does a press
        // on the corner of the bounding square.
        QTest::mousePress(gear, Qt::LeftButton, Qt::NoModifier, gear->rect().center());
        QTest::mouseRelease(gear, Qt::LeftButton, Qt::NoModifier, QPoint{1, 1});
        QCOMPARE(settingsRequested.count(), 1);
        QTest::mouseClick(gear, Qt::LeftButton, Qt::NoModifier, QPoint{1, 1});
        QCOMPARE(settingsRequested.count(), 1);

        gear->setFocus();
        QTest::keyClick(gear, Qt::Key_Space);
        QCOMPARE(settingsRequested.count(), 2);

        // The close button only hides the window. The window may be sliding out by now, so its
        // visibility is deliberately not checked.
        QTest::mouseClick(close, Qt::LeftButton, Qt::NoModifier, close->rect().center());
        QCOMPARE(settingsRequested.count(), 2);
    }

    void MainWindowHeaderRendersInBothThemes()
    {
        ThemeModeGuard modeGuard;
        auto &theme = Gui::Theme::Manager::Instance();
        const auto state = AirPods5State();
        const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation");
        QVERIFY(QDir{}.mkpath(outputDir));

        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            theme.SetMode(mode);
            Gui::MainWindow window;
            window.UpdateState(state);
            window.Show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));

            auto *animation = window.findChild<Gui::Widget::AnimationView *>();
            QVERIFY(animation != nullptr);
            QTRY_VERIFY(animation->isVisible());
            QVERIFY(window.findChild<QAbstractButton *>("settingsButton") != nullptr);
            QVERIFY(window.findChild<QAbstractButton *>("closeButton") != nullptr);

            const auto image = window.grab();
            QVERIFY(!image.isNull());
            const auto fileName =
                QString{"/main-window-header-%1.png"}.arg(theme.IsDark() ? "dark" : "light");
            QVERIFY(image.save(outputDir + fileName));
            window.close();
        }
    }

    void GearOpensSettingsThroughTray()
    {
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};
        Gui::TrayIcon tray{[] { return 0; }, quickConnect};
        Gui::MainWindow window;
        connect(&window, &Gui::MainWindow::SettingsRequested, &tray, &Gui::TrayIcon::ShowSettings);
        window.Show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // The tray owns its settings window by value, without a parent, so `findChild` cannot
        // reach it. Search the top-level windows instead.
        const auto findSettingsWindow = []() -> QWidget * {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (widget->objectName() == QStringLiteral("SettingsWindow")) {
                    return widget;
                }
            }
            return nullptr;
        };
        QVERIFY(findSettingsWindow() != nullptr);
        QVERIFY(!findSettingsWindow()->isVisible());

        auto *gear = window.findChild<QAbstractButton *>("settingsButton");
        QVERIFY(gear != nullptr);
        QTest::mouseClick(gear, Qt::LeftButton, Qt::NoModifier, gear->rect().center());

        auto *settings = findSettingsWindow();
        QVERIFY(settings != nullptr);
        QTRY_VERIFY(settings->isVisible());
        auto *navigation = settings->findChild<QListWidget *>("navList");
        QVERIFY(navigation != nullptr);
        QCOMPARE(navigation->currentRow(), 0);

        // Asking again while the About page is showing returns to the first page, exactly like
        // the Settings entry of the tray menu.
        navigation->setCurrentRow(3);
        QCOMPARE(navigation->currentRow(), 3);
        tray.ShowSettings();
        QCOMPARE(navigation->currentRow(), 0);

        settings->hide();
    }

    void PaletteMeetsContrastTargets_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void PaletteMeetsContrastTargets()
    {
        QFETCH(bool, dark);
        ThemeModeGuard modeGuard;
        auto &theme = Gui::Theme::Manager::Instance();
        theme.SetMode(dark ? Gui::Theme::Mode::Dark : Gui::Theme::Mode::Light);
        QCOMPARE(theme.IsDark(), dark);
        // A copy, because the manager rebuilds its palette in place when the theme changes.
        const auto colors = theme.Colors();
        const QColor white{Qt::white};

        QStringList shortfalls;
        const auto require = [&](const char *what, const QColor &foreground,
                                 const QColor &background, qreal minimum) {
            const auto ratio = ContrastRatio(foreground, background);
            if (ratio < minimum) {
                shortfalls.push_back(QStringLiteral("%1: %2 is below %3")
                                         .arg(
                                             QString::fromLatin1(what),
                                             QString::number(ratio, 'f', 2),
                                             QString::number(minimum, 'f', 1)));
            }
        };

        // Body text
        require("text on windowBackground", colors.text, colors.windowBackground, 7.0);
        require("text on surface", colors.text, colors.surface, 7.0);
        require("text on surfaceSecondary", colors.text, colors.surfaceSecondary, 7.0);
        require(
            "textSecondary on windowBackground", colors.textSecondary, colors.windowBackground,
            4.5);
        require("textSecondary on surface", colors.textSecondary, colors.surface, 4.5);
        require(
            "textSecondary on surfaceSecondary", colors.textSecondary, colors.surfaceSecondary,
            4.5);
        require("errorText on windowBackground", colors.errorText, colors.windowBackground, 4.5);
        require("errorText on surface", colors.errorText, colors.surface, 4.5);

        // Filled accent controls: text on the default button, and the white thumb of a switch
        require("accentText on accent", colors.accentText, colors.accent, 4.5);
        require("white on accentFill", white, colors.accentFill, 3.0);

        // Sidebar and switch
        require("text on sidebarSelected", colors.text, colors.sidebarSelected, 7.0);
        require("switchOff on surface", colors.switchOff, colors.surface, 1.5);
        require("switchOff against the white thumb", colors.switchOff, white, 1.5);

        // Main window
        require("mainCloseGlyph on mainCloseBg", colors.mainCloseGlyph, colors.mainCloseBg, 4.0);
        require("batteryAlarm on mainSurface", colors.batteryAlarm, colors.mainSurface, 3.0);

        QVERIFY2(shortfalls.isEmpty(), qPrintable(shortfalls.join(QStringLiteral("\n"))));

        QVERIFY(colors.accentText == QColor{Qt::black} || colors.accentText == QColor{Qt::white});

        // The selected row has to stand out from the sidebar without turning into a button.
        const auto selection = ContrastRatio(colors.sidebarSelected, colors.surfaceSecondary);
        QVERIFY2(selection >= 1.1 && selection <= 1.6, qPrintable(QString::number(selection)));

        // Only the translucent rim has alpha. Stylesheets and surfaces must stay opaque.
        QCOMPARE(colors.mainSurface.alpha(), 255);
        QCOMPARE(colors.popupSurface.alpha(), 255);
        QVERIFY(colors.mainRim.alpha() > 0 && colors.mainRim.alpha() < 255);
    }

    void GlyphsStayInsideTheirBox()
    {
        struct GlyphCase {
            Gui::Glyph::Kind kind;
            const char *name;
        };
        constexpr GlyphCase glyphs[]{
            {Gui::Glyph::Kind::Close, "Close"},       {Gui::Glyph::Kind::Gear, "Gear"},
            {Gui::Glyph::Kind::General, "General"},   {Gui::Glyph::Kind::Visual, "Visual"},
            {Gui::Glyph::Kind::Features, "Features"}, {Gui::Glyph::Kind::About, "About"},
            {Gui::Glyph::Kind::Debug, "Debug"},
        };

        for (const auto &glyph : glyphs) {
            for (const qreal ratio : {1.0, 1.25, 1.5, 2.0}) {
                const auto label =
                    QStringLiteral("%1 at %2x")
                        .arg(QString::fromLatin1(glyph.name), QString::number(ratio));

                // 48 logical pixels with the 32 pixel box in the middle leave an 8 pixel margin
                // that has to stay empty, antialiasing included.
                const int side = qRound(48 * ratio);
                QImage image{side, side, QImage::Format_ARGB32_Premultiplied};
                image.setDevicePixelRatio(ratio);
                image.fill(Qt::transparent);
                {
                    QPainter painter{&image};
                    painter.setRenderHint(QPainter::Antialiasing);
                    Gui::Glyph::Draw(painter, glyph.kind, QRectF{8, 8, 32, 32}, Qt::black);
                }

                const int boxStart = qRound(8 * ratio);
                const int boxEnd = qRound(40 * ratio);
                int inside = 0;
                int outside = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (qAlpha(image.pixel(x, y)) == 0) {
                            continue;
                        }
                        if (x >= boxStart && x < boxEnd && y >= boxStart && y < boxEnd) {
                            ++inside;
                        }
                        else {
                            ++outside;
                        }
                    }
                }
                QVERIFY2(inside >= 40, qPrintable(label + QStringLiteral(": nothing drawn")));
                QVERIFY2(
                    outside == 0,
                    qPrintable(label + QStringLiteral(": %1 pixels outside").arg(outside)));
            }
        }
    }

    void SwitchBehavesLikeACheckBox()
    {
        MemorySettingsScope settings;
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};
        Gui::SettingsWindow window{[] { return 0; }, quickConnect};
        window.resize(640, 420);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        // The Features page
        window.SetTabIndex(2);
        auto *toggle = window.findChild<QCheckBox *>("cbLowAudioLatency");
        QVERIFY(toggle != nullptr);
        QTRY_VERIFY(toggle->isVisible());
        toggle->ensurePolished();

        // The keyboard works exactly like it does for a plain check box.
        toggle->setFocus();
        QTRY_VERIFY(toggle->hasFocus());
        QSignalSpy toggled{toggle, &QCheckBox::toggled};
        const bool initiallyChecked = toggle->isChecked();
        QTest::keyClick(toggle, Qt::Key_Space);
        QCOMPARE(toggle->isChecked(), !initiallyChecked);
        QTest::keyClick(toggle, Qt::Key_Space);
        QCOMPARE(toggle->isChecked(), initiallyChecked);
        QCOMPARE(toggled.count(), 2);

        // The track is 40x24 and does not change size when it takes the focus.
        const auto indicatorRect = [toggle] {
            QStyleOptionButton option;
            option.initFrom(toggle);
            return toggle->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, toggle);
        };
        QStyleOptionButton focusedOption;
        focusedOption.initFrom(toggle);
        QVERIFY(focusedOption.state.testFlag(QStyle::State_HasFocus));
        const auto focused = indicatorRect();

        toggle->clearFocus();
        QVERIFY(!toggle->hasFocus());
        const auto unfocused = indicatorRect();

        QCOMPARE(unfocused.size(), QSize(40, 24));
        QCOMPARE(focused.size(), unfocused.size());
    }

    void SettingsWindowUsesGroupedRows()
    {
        ThemeModeGuard modeGuard;
        auto backend = std::make_shared<Core::QuickConnect::NullBackend>();
        Core::QuickConnect::Controller quickConnect{backend};
        Gui::SettingsWindow window{[] { return 0; }, quickConnect};
        window.resize(640, 420);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // General: language + auto run + receiving range, device + diagnostics.
        // Visual: appearance, tray icon battery + taskbar battery.
        // Features: low audio latency + ear detection + show on connect, quick connect.
        int groupCount = 0;
        for (const auto *frame : window.findChildren<QFrame *>()) {
            if (CssClass(frame) == QStringLiteral("settingGroup")) {
                ++groupCount;
            }
        }
        QCOMPARE(groupCount, 6);

        // The pages that use groups no longer hold stand-alone cards. The About page still does.
        for (const auto *pageName : {"general", "visual", "features"}) {
            auto *page = window.findChild<QWidget *>(pageName);
            QVERIFY2(page != nullptr, pageName);
            for (const auto *frame : page->findChildren<QFrame *>()) {
                QVERIFY2(
                    CssClass(frame) != QStringLiteral("settingCard"),
                    qPrintable(frame->objectName()));
            }
        }

        auto *navigation = window.findChild<QListWidget *>("navList");
        QVERIFY(navigation != nullptr);
        QCOMPARE(navigation->iconSize(), QSize(18, 18));
        for (int row = 0; row < 4; ++row) {
            const auto icon = navigation->item(row)->icon();
            QVERIFY2(!icon.isNull(), qPrintable(QString::number(row)));
            QVERIFY2(
                HasVisiblePixel(icon.pixmap(QSize{18, 18}).toImage()),
                qPrintable(QString::number(row)));
        }

        const auto outputDir = QStringLiteral(APD_BINARY_DIR "/UiValidation");
        QVERIFY(QDir{}.mkpath(outputDir));
        auto &theme = Gui::Theme::Manager::Instance();
        for (const auto mode : {Gui::Theme::Mode::Light, Gui::Theme::Mode::Dark}) {
            theme.SetMode(mode);
            QCoreApplication::processEvents();
            const auto fileName =
                QString{"/settings-grouped-%1.png"}.arg(theme.IsDark() ? "dark" : "light");
            QVERIFY(window.grab().save(outputDir + fileName));
        }
    }
};

int main(int argc, char **argv)
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app{argc, argv};
    // Production logging installs a Qt message handler during static initialization. Restore the
    // default handler so QTest assertion details remain visible in CTest and CI output.
    qInstallMessageHandler(nullptr);
    Gui::Theme::ApplyApplicationTypography(QLocale{"en_US"});
    UiRenderingTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "UiRenderingTests.moc"
