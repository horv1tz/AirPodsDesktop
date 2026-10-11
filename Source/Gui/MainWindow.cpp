//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "MainWindow.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QScreen>
#include <QCursor>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QMessageBox>
#include <QCloseEvent>

#include <Config.h>
#include "../Helper.h"
#include "../Error.h"
#include "../Core/AppleCP.h"
#include "../Core/Settings.h"
#include "UpdateWindow.h"
#include "SelectWindow.h"
#include "Glyph.h"
#include "Theme.h"

using namespace std::chrono_literals;

namespace Gui {

// A round header button that draws one of the vector glyphs. It is a real `QAbstractButton`, so it
// gets the keyboard activation, the accessibility role and the `clicked` signal for free.
class GlyphButton : public QAbstractButton
{
    Q_OBJECT

public:
    GlyphButton(Glyph::Kind kind, QWidget *parent) : QAbstractButton{parent}, _kind{kind}
    {
        setFixedSize(28, 28);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

protected:
    // Only the circle reacts, not the corners of the bounding square.
    bool hitButton(const QPoint &p) const override
    {
        return QLineF{QRectF{rect()}.center(), QPointF{p}}.length() <= width() / 2.0;
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        const auto &colors = Theme::Manager::Instance().Colors();

        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);

        painter.setPen(Qt::NoPen);
        if (isDown()) {
            painter.setBrush(colors.mainClosePressed);
        }
        else if (underMouse()) {
            painter.setBrush(colors.mainCloseHover);
        }
        else {
            painter.setBrush(colors.mainCloseBg);
        }
        painter.drawEllipse(rect());

        constexpr qreal glyphSize = 14.0;
        Glyph::Draw(
            painter, _kind,
            QRectF{(width() - glyphSize) / 2.0, (height() - glyphSize) / 2.0, glyphSize, glyphSize},
            colors.mainCloseGlyph);

        if (hasFocus() && _keyboardFocus) {
            painter.setPen(QPen{colors.accent, 2.0});
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF{rect()}.adjusted(1, 1, -1, -1));
        }
    }

    // The ring is for keyboard navigation only, not for automatic focus on window activation.
    void focusInEvent(QFocusEvent *event) override
    {
        _keyboardFocus = event->reason() == Qt::TabFocusReason ||
                         event->reason() == Qt::BacktabFocusReason ||
                         event->reason() == Qt::ShortcutFocusReason;
        QAbstractButton::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        _keyboardFocus = false;
        QAbstractButton::focusOutEvent(event);
        update();
    }

private:
    Glyph::Kind _kind;
    bool _keyboardFocus = false;
};

//////////////////////////////////////////////////

MainWindow::MainWindow(QWidget *parent) : QDialog{parent}
{
    qRegisterMetaType<Core::AirPods::State>("Core::AirPods::State");
    qRegisterMetaType<Core::Update::ReleaseInfo>("Core::Update::ReleaseInfo");

    _animationView = new Widget::AnimationView{this};
    _playback = new AnimationPlayback{*_animationView, this};

    _ui.setupUi(this);

    setFixedSize(_windowSize);
    setWindowFlags(windowFlags() | Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    // A translucent backing store retains fractional edge alpha. Native window regions and
    // QBitmap masks are binary and produce visibly stepped corners, especially above 100% DPI.
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setProperty(Theme::Manager::kSkipDwmProperty, true);

    // Both buttons sit on the card corners, on the centres of the corner arcs. They are created
    // after `setupUi` and raised, so no sibling can cover them.
    _settingsButton = new GlyphButton{Glyph::Kind::Gear, this};
    _settingsButton->setObjectName("settingsButton");
    _closeButton = new GlyphButton{Glyph::Kind::Close, this};
    _closeButton->setObjectName("closeButton");
    _settingsButton->move(_headerInset, _headerInset);
    _closeButton->move(width() - _headerInset - _closeButton->width(), _headerInset);
    _settingsButton->raise();
    _closeButton->raise();
    setTabOrder(_settingsButton, _closeButton);
    setTabOrder(_closeButton, _ui.pushButton);

    _ui.pushButton->setProperty("cssClass", "accent");

    auto titleFont = _ui.deviceLabel->font();
    titleFont.setWeight(QFont::DemiBold);
    _ui.deviceLabel->setFont(titleFont);
    _ui.deviceLabel->setTextFormat(Qt::PlainText);
    _ui.deviceLabel->setWordWrap(false);
    _ui.deviceLabel->setProperty("fontRole", "display");

    ApplyTheme();
    connect(&Theme::Manager::Instance(), &Theme::Manager::Changed, this, &MainWindow::ApplyTheme);

    connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::OnAppStateChanged);
    connect(_ui.pushButton, &QPushButton::clicked, this, &MainWindow::OnButtonClicked);
    connect(&_posAnimation, &QPropertyAnimation::finished, this, &MainWindow::OnPosMoveFinished);
    connect(_animationView, &Widget::AnimationView::Clicked, this, &MainWindow::OnAnimationClicked);
    connect(_closeButton, &QAbstractButton::clicked, this, &MainWindow::DoHide);
    connect(_settingsButton, &QAbstractButton::clicked, this, [this] {
        // Settings first, so it is already showing when the always-on-top popup slides away.
        emit SettingsRequested();
        DoHide();
    });

    connect(this, &MainWindow::UpdateStateSafely, this, &MainWindow::UpdateState);
    connect(this, &MainWindow::AvailableSafely, this, &MainWindow::Available);
    connect(this, &MainWindow::UnavailableSafely, this, &MainWindow::Unavailable);
    connect(this, &MainWindow::DisconnectSafely, this, &MainWindow::Disconnect);
    connect(this, &MainWindow::BindSafely, this, &MainWindow::Bind);
    connect(this, &MainWindow::UnbindSafely, this, &MainWindow::Unbind);
    connect(this, &MainWindow::ShowSafely, this, &MainWindow::Show);
    connect(this, &MainWindow::HideSafely, this, &MainWindow::DoHide);
    connect(
        this, &MainWindow::VersionUpdateAvailableSafely, this, &MainWindow::VersionUpdateAvailable);

    _posAnimation.setDuration(500);
    _autoHideTimer->callOnTimeout([this] { DoHide(); });

    _ui.layoutAnimation->addWidget(_animationView);
    _ui.layoutPods->addWidget(_leftBattery);
    _ui.layoutPods->addWidget(_rightBattery);
    _ui.layoutCase->addWidget(_caseBattery);

    // For getting the correct initial height of `_animationView` later
    _ui.layoutAnimation->activate();
    _animationView->show();

    ApplyHeaderTexts();
}

void MainWindow::StartUpdateChecks()
{
    _updateChecker.Start();
}

MainWindow::~MainWindow()
{
    // Stop the decoder before QObject destroys the video surface child.
    delete _playback;
    _deviceQueryThread.request_stop();
    if (_deviceQueryThread.joinable()) {
        _deviceQueryThread.join();
    }
}

void MainWindow::UpdateState(const Core::AirPods::State &state)
{
    LOG(Info, "MainWindow::UpdateState");

    _viewModel.UpdateState(state);
    Repaint();
}

void MainWindow::Available()
{
    LOG(Info, "MainWindow::Available");

    _viewModel.Available();
    Repaint();
}

void MainWindow::Unavailable()
{
    LOG(Info, "MainWindow::Unavailable");

    _viewModel.Unavailable();
    Repaint();
}

void MainWindow::Disconnect()
{
    LOG(Info, "MainWindow::Disconnect");

    _viewModel.Disconnect();
    Repaint();
}

void MainWindow::Bind()
{
    LOG(Info, "MainWindow::Bind");

    _viewModel.Bind();
    Repaint();
}

void MainWindow::Unbind()
{
    LOG(Info, "MainWindow::Unbind");

    _viewModel.Unbind();
    Repaint();
}

void MainWindow::AskUserUpdate(const Core::Update::ReleaseInfo &releaseInfo)
{
    auto releaseVersion = releaseInfo.version.toString();

    UpdateWindow updateWindow{releaseInfo};
    updateWindow.exec();
    const auto action = updateWindow.SelectedAction();
    switch (action) {
    case UpdateWindow::Action::Update:
        LOG(Info, "VersionUpdate: User clicked Update.");

        if (updateWindow.Result() == UpdateWindow::Outcome::KeepRunning) {
            return;
        }

        Utils::Qt::QuitApplicationSafely();
        return;

    case UpdateWindow::Action::Skip:
        LOG(Info, "VersionUpdate: User clicked Skip.");

        Core::Settings::ModifiableAccess()->skipped_version = releaseVersion;

        // Continue checking for new versions after the skipped version
        break;

    case UpdateWindow::Action::Later:
        LOG(Info, "VersionUpdate: User clicked Later.");

        _updateChecker.Stop();
        break;

    default:
        LOG(Warn, "VersionUpdate: Unhandled user clicked button.");
        break;
    }
}

void MainWindow::ChangeButtonAction(ButtonAction action)
{
    switch (action) {
    case ButtonAction::NoButton:
        _ui.pushButton->setText("");
        _ui.pushButton->hide();
        return;

    case ButtonAction::Bind:
        _ui.pushButton->setText(tr("Bind to AirPods"));
        break;

    default:
        FatalError(std::format("Unhandled ButtonAction: '{}'", Helper::ToUnderlying(action)), true);
    }

    _buttonAction = action;
    _ui.pushButton->show();
}

void MainWindow::SetAnimation(std::optional<Core::AirPods::Model> model)
{
    if (model == _cacheModel) {
        return;
    }

    if (!model.has_value()) {
        StopAnimation();
        _playback->SetAnimation({});
    }
    else {
        const auto presentation = GetAnimationPresentation(model.value());

        auto aspectRatio =
            (float)presentation.sourceSize.width() / (float)presentation.sourceSize.height();
        auto widgetWidth = _animationView->height() * aspectRatio;
        _animationView->setFixedWidth(widgetWidth);
        _playback->SetAnimation(presentation);

        if (_isVisible) {
            PlayAnimation();
        }
        else {
            StopAnimation();
        }
    }

    _cacheModel = model;
}

void MainWindow::PlayAnimation()
{
    _playback->SetActive(true);
    _animationView->show();
}

void MainWindow::StopAnimation()
{
    _animationView->hide();
    _playback->SetActive(false);
}

void MainWindow::BindDevice()
{
    LOG(Info, "BindDevice");

    if (_deviceQueryRunning.exchange(true)) {
        LOG(Info, "Ignore duplicate device query while one is already running.");
        return;
    }

    if (_deviceQueryThread.joinable()) {
        _deviceQueryThread.join();
    }

    _deviceQueryThread = std::jthread{[this](std::stop_token stopToken) {
        Core::OS::Windows::Winrt::Initialize();
        auto devices = Core::AirPods::GetDevices();
        if (stopToken.stop_requested()) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, devices = std::move(devices)]() mutable {
                _deviceQueryRunning = false;
                ShowDeviceSelector(std::move(devices));
            },
            Qt::QueuedConnection);
    }};
}

void MainWindow::ShowDeviceSelector(std::vector<Core::Bluetooth::Device> devices)
{
    if (devices.empty()) {
        QMessageBox::warning(
            this, Config::ProgramName,
            QMessageBox::tr("No paired device found.\n"
                            "You need to pair your AirPods in Windows Bluetooth Settings first."));
        return;
    }

    int selectedIndex = 0;

    if (devices.size() > 1) {
        QStringList deviceNames;
        for (const auto &device : devices) {
            auto deviceName = device.GetName();

            LOG(Trace, "Device name: '{}'", deviceName);
            LOG(Trace, "GetProductId: '{}' GetVendorId: '{}'", device.GetProductId(),
                device.GetVendorId());
            deviceNames.append(QString::fromStdString(deviceName));
        }

        SelectWindow selector{tr("Please select your AirPods device below."), deviceNames, this};
        if (selector.exec() == -1) {
            LOG(Warn, "selector.exec() == -1");
            return;
        }

        if (!selector.HasResult()) {
            LOG(Info, "No result for selector.");
            return;
        }

        selectedIndex = selector.GetSeletedIndex();
        APD_ASSERT(selectedIndex >= 0 && selectedIndex < devices.size());
    }

    const auto &selectedDevice = devices.at(selectedIndex);

    LOG(Info, "Selected device index: '{}', device name: '{}'. Bound to this device.",
        selectedIndex, selectedDevice.GetName());

    Core::Settings::ModifiableAccess()->device_address = selectedDevice.GetAddress();
}

void MainWindow::ControlAutoHideTimer(bool start)
{
    LOG(Trace, "ControlAutoHideTimer: start == '{}', _isVisible == '{}'", start, _isVisible);

    if (start && _isVisible) {
        _autoHideTimer->start(10s);
    }
    else {
        _autoHideTimer->stop();
    }
}

void MainWindow::VersionUpdateAvailable(const Core::Update::ReleaseInfo &releaseInfo, bool silent)
{
    LOG(Info, "MainWindow::VersionUpdateAvailable: silent: `{}`", silent);

    if (!silent) {
        AskUserUpdate(releaseInfo);
    }
    else {
        emit SilentUpdateAvailable(releaseInfo);
    }
}

void MainWindow::Repaint()
{
    const auto presentation = _viewModel.Present();
    FitDeviceLabelFont(presentation.title);
    ChangeButtonAction(presentation.buttonAction);
    SetAnimation(presentation.animationModel);

    const auto applyBattery = [](Widget::Battery *widget, const BatteryPresentation &battery) {
        if (!battery.visible) {
            widget->hide();
            return;
        }

        widget->setCharging(battery.charging);
        widget->setValue(battery.value);
        widget->show();
    };

    applyBattery(_leftBattery, presentation.leftBattery);
    applyBattery(_rightBattery, presentation.rightBattery);
    applyBattery(_caseBattery, presentation.caseBattery);
}

void MainWindow::ApplyTheme()
{
    const auto &colors = Theme::Manager::Instance().Colors();

    Utils::Qt::SetPaletteColor(this, QPalette::Window, colors.mainSurface);
    Utils::Qt::SetPaletteColor(_ui.deviceLabel, QPalette::WindowText, colors.mainText);

    for (auto *battery : {_leftBattery, _rightBattery, _caseBattery}) {
        battery->setNormalColor(colors.batteryNormal);
        battery->setAlarmColor(colors.batteryAlarm);
        battery->setBorderColor(colors.batteryBorder);
        battery->setChargingIconColor(colors.mainText);
        battery->setBorderRadius(4);
        battery->setBackgroundRadius(2.5);
        battery->setHeadRadius(1.5);
        Utils::Qt::SetPaletteColor(battery, QPalette::WindowText, colors.mainText);
    }

    _settingsButton->update();
    _closeButton->update();
    update();
}

void MainWindow::ApplyHeaderTexts()
{
    // Both strings are already translated for the tray menu and the Settings window, so the
    // header buttons reuse those contexts instead of adding new sources.
    if (_settingsButton != nullptr) {
        const auto text = QCoreApplication::translate("Gui::TrayIcon", "Settings");
        _settingsButton->setToolTip(text);
        _settingsButton->setAccessibleName(text);
    }
    if (_closeButton != nullptr) {
        const auto text = QCoreApplication::translate("Gui::SettingsWindow", "Close");
        _closeButton->setToolTip(text);
        _closeButton->setAccessibleName(text);
    }
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    const auto &colors = Theme::Manager::Instance().Colors();
    // One device pixel, so the rim stays a crisp hairline at every scale factor.
    const qreal px = 1.0 / devicePixelRatioF();
    const QRectF card{rect()};

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    // The card itself is opaque, which keeps the corner pixels near the edge fully covered.
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.mainSurface);
    painter.drawRoundedRect(card, _windowCornerRadius, _windowCornerRadius);

    // The translucent rim takes the place of a window shadow. The stroke is centred half a pixel
    // inside the edge, so it covers exactly the outermost device pixel.
    painter.setPen(QPen{colors.mainRim, px});
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(
        card.adjusted(px / 2, px / 2, -px / 2, -px / 2), _windowCornerRadius - px / 2,
        _windowCornerRadius - px / 2);
}

void MainWindow::FitDeviceLabelFont(const QString &text)
{
    auto font = _ui.deviceLabel->font();
    font.setPointSize(_deviceLabelMaximumPointSize);

    const auto availableWidth = _ui.deviceLabel->contentsRect().width();
    while (font.pointSize() > _deviceLabelMinimumPointSize &&
           QFontMetrics{font}.horizontalAdvance(text) > availableWidth)
    {
        font.setPointSize(font.pointSize() - 1);
    }

    _ui.deviceLabel->setFont(font);
    const QFontMetrics metrics{font};
    const auto displayText = metrics.horizontalAdvance(text) > availableWidth
                                 ? metrics.elidedText(text, Qt::ElideMiddle, availableWidth)
                                 : text;
    _ui.deviceLabel->setText(displayText);
    _ui.deviceLabel->setAccessibleName(text);
    _ui.deviceLabel->setToolTip(displayText == text ? QString{} : text);
}

void MainWindow::OnAppStateChanged(Qt::ApplicationState state)
{
    LOG(Trace, "OnAppStateChanged: '{}'", Helper::ToString(state));
    ControlAutoHideTimer(_isVisible && state != Qt::ApplicationActive);
}

void MainWindow::OnPosMoveFinished()
{
    if (!_isVisible) {
        hide();
    }
}

void MainWindow::OnAnimationClicked()
{
#if defined APD_DEBUG
    using namespace Core::AirPods;

    static Model next = Model::AirPods_1;

    _ui.deviceLabel->setText(Helper::ToString(next));
    SetAnimation(next);

    next = static_cast<Model>(Helper::ToUnderlying(next) + 1);
    if (next >= Model::_Max) {
        next = Model::AirPods_1;
    }
#endif
}

void MainWindow::OnButtonClicked()
{
    switch (_buttonAction) {
    case ButtonAction::Bind:
        LOG(Info, "User clicked 'Bind'");
        BindDevice();
        break;

    default:
        FatalError(
            std::format("Unhandled ButtonAction: '{}'", Helper::ToUnderlying(_buttonAction)), true);
    }
}

void MainWindow::DoHide()
{
    LOG(Trace, "MainWindow: Hide");

    if (!_isVisible) {
        return;
    }
    _isVisible = false;

    ControlAutoHideTimer(false);

    const auto screenGeometry = screen()->geometry();

    _posAnimation.stop();
    _posAnimation.setEasingCurve(QEasingCurve::InExpo);
    _posAnimation.setStartValue(pos());
    _posAnimation.setEndValue(QPoint{x(), screenGeometry.bottom() + 1});
    _posAnimation.start();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    BeginShow(true);
}

void MainWindow::Show()
{
    if (isVisible()) {
        BeginShow(false);
    }
    else {
        QDialog::show();
    }
}

void MainWindow::BeginShow(bool fromHidden)
{
    LOG(Trace, "MainWindow: Show");

    auto targetScreen = QGuiApplication::screenAt(QCursor::pos());
    if (targetScreen == nullptr) {
        targetScreen = screen();
    }

    const auto availableGeometry = targetScreen->availableGeometry();
    const auto screenGeometry = targetScreen->geometry();
    const auto target = PopupPosition(availableGeometry, size(), _screenMargin);
    const bool changingScreen = screen() != targetScreen;

    if (changingScreen && !fromHidden) {
        // Hide before the native DPI transition so an intermediate resize is never painted.
        hide();
        QDialog::show();
        return;
    }

    if (_isVisible && !changingScreen &&
        (pos() == target || (_posAnimation.state() == QAbstractAnimation::Running &&
                             _posAnimation.endValue().toPoint() == target)))
    {
        ControlAutoHideTimer(true);
        return;
    }
    _isVisible = true;
    PlayAnimation();
    ControlAutoHideTimer(true);

    _posAnimation.stop();
    if (fromHidden || changingScreen) {
        // Resolve the native monitor and its DPI inside the destination screen before
        // positioning the slide origin outside it.
        move(target);
        move(target.x(), screenGeometry.bottom() + 1);
    }
    _posAnimation.setEasingCurve(QEasingCurve::OutExpo);
    _posAnimation.setStartValue(pos());
    _posAnimation.setEndValue(target);
    _posAnimation.start();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    _isVisible = false;
    _posAnimation.stop();
    ControlAutoHideTimer(false);
    StopAnimation();
    QDialog::hideEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    DoHide();
}
} // namespace Gui

#include "MainWindow.moc"
