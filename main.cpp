#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <optional>

using BluezInterfaceMap = QMap<QString, QVariantMap>;
using BluezManagedObjects = QMap<QDBusObjectPath, BluezInterfaceMap>;

Q_DECLARE_METATYPE(BluezInterfaceMap)
Q_DECLARE_METATYPE(BluezManagedObjects)

static constexpr const char *kDefaultSysfs =
	"/sys/devices/platform/pmic-glink/pmic_glink.power-supply.0/xiaomi";
static constexpr const char *kFocusPenProName = "Xiaomi Focus Pen Pro";
static constexpr const char *kFocusPenCommandUuid =
	"0000fe11-aa6c-462a-964a-7f2ed5b3e512";
static constexpr const char *kFocusPenProReadyPath =
	"/run/xiaomi-sheng-thp/p81c-fe11-ready";

static QByteArray focusPenProPinchLevelCommand(int level)
{
	static constexpr std::array<std::array<quint16, 2>, 5> thresholds = {{
		{{90, 45}}, {{180, 90}}, {{270, 135}}, {{360, 180}}, {{450, 225}},
	}};
	const auto values = thresholds.at(
		static_cast<size_t>(qBound(1, level, 5) - 1));
	QByteArray command;
	command.reserve(6);
	command.append(char(0x5c));
	command.append(char(0x04));
	for (quint16 value : values) {
		command.append(char(value >> 8));
		command.append(char(value & 0xff));
	}
	return command;
}

struct BluetoothState {
	bool serviceAvailable = false;
	bool powered = false;
	bool discovering = false;
	bool deviceFound = false;
	bool paired = false;
	bool trusted = false;
	bool connected = false;
	bool servicesResolved = false;
	bool focusPenPro = false;
	QString adapterPath;
	QString devicePath;
	QString commandPath;
	QString firmwareRevision;
	QString softwareRevision;
};

static QByteArray readGattValue(const QString &path, const QVariantMap &properties)
{
	QByteArray value = properties.value(QStringLiteral("Value")).toByteArray();
	if (!value.isEmpty())
		return value;

	QDBusInterface characteristic(QStringLiteral("org.bluez"), path,
					 QStringLiteral("org.bluez.GattCharacteristic1"),
					 QDBusConnection::systemBus());
	if (!characteristic.isValid())
		return {};
	const QDBusReply<QByteArray> reply = characteristic.call(
		QStringLiteral("ReadValue"), QVariantMap{});
	return reply.isValid() ? reply.value() : QByteArray{};
}

static QString decodeGattText(QByteArray value)
{
	const qsizetype terminator = value.indexOf('\0');
	if (terminator >= 0)
		value.truncate(terminator);
	return QString::fromUtf8(value).trimmed();
}

static BluetoothState readBluetoothState(const QString &address)
{
	static const bool registered = []() {
		qDBusRegisterMetaType<BluezInterfaceMap>();
		qDBusRegisterMetaType<BluezManagedObjects>();
		return true;
	}();
	Q_UNUSED(registered);

	BluetoothState state;
	QDBusInterface manager(QStringLiteral("org.bluez"), QStringLiteral("/"),
				       QStringLiteral("org.freedesktop.DBus.ObjectManager"),
				       QDBusConnection::systemBus());
	if (!manager.isValid())
		return state;

	const QDBusReply<BluezManagedObjects> reply = manager.call(QStringLiteral("GetManagedObjects"));
	if (!reply.isValid())
		return state;
	state.serviceAvailable = true;

	const BluezManagedObjects objects = reply.value();
	for (auto object = objects.cbegin(); object != objects.cend(); ++object) {
		const auto adapter = object.value().constFind(QStringLiteral("org.bluez.Adapter1"));
		if (adapter != object.value().cend() && state.adapterPath.isEmpty()) {
			state.adapterPath = object.key().path();
			state.powered = adapter->value(QStringLiteral("Powered")).toBool();
			state.discovering = adapter->value(QStringLiteral("Discovering")).toBool();
		}

		const auto device = object.value().constFind(QStringLiteral("org.bluez.Device1"));
		if (device == object.value().cend())
			continue;
		if (device->value(QStringLiteral("Address")).toString().compare(
				address, Qt::CaseInsensitive) != 0)
			continue;

		state.deviceFound = true;
		state.devicePath = object.key().path();
		state.paired = device->value(QStringLiteral("Paired")).toBool();
		state.trusted = device->value(QStringLiteral("Trusted")).toBool();
		state.connected = device->value(QStringLiteral("Connected")).toBool();
		state.servicesResolved =
			device->value(QStringLiteral("ServicesResolved")).toBool();
		state.focusPenPro = device->value(QStringLiteral("Name")).toString() ==
			QString::fromUtf8(kFocusPenProName);
	}

	if (!state.connected || state.devicePath.isEmpty())
		return state;
	const QString devicePrefix = state.devicePath + QLatin1Char('/');
	for (auto object = objects.cbegin(); object != objects.cend(); ++object) {
		if (!object.key().path().startsWith(devicePrefix))
			continue;
		const auto characteristic = object.value().constFind(
			QStringLiteral("org.bluez.GattCharacteristic1"));
		if (characteristic == object.value().cend())
			continue;
		const QString uuid = characteristic->value(QStringLiteral("UUID")).toString();
		if (uuid.compare(QString::fromUtf8(kFocusPenCommandUuid),
				 Qt::CaseInsensitive) == 0) {
			state.commandPath = object.key().path();
		} else if (uuid.compare(QStringLiteral("00002a26-0000-1000-8000-00805f9b34fb"),
				 Qt::CaseInsensitive) == 0) {
			state.firmwareRevision = decodeGattText(
				readGattValue(object.key().path(), *characteristic));
		} else if (uuid.compare(
				   QStringLiteral("00002a28-0000-1000-8000-00805f9b34fb"),
				   Qt::CaseInsensitive) == 0) {
			state.softwareRevision = decodeGattText(
				readGattValue(object.key().path(), *characteristic));
		}
	}
	return state;
}

static std::optional<int> readInt(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return std::nullopt;

	bool ok = false;
	const int value = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok, 0);
	if (!ok)
		return std::nullopt;

	return value;
}

static QString readText(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return {};
	return QString::fromUtf8(file.readAll()).trimmed();
}

static std::optional<quint32> readHex32(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return std::nullopt;

	bool ok = false;
	const quint32 value = QString::fromUtf8(file.readAll()).trimmed().toUInt(&ok, 16);
	if (!ok)
		return std::nullopt;
	return value;
}

static bool useChinese()
{
	const QByteArray lang = qgetenv("XIAOMI_PEN_LANG").toLower();
	if (lang == "zh" || lang == "zh_cn" || lang == "cn")
		return true;
	if (lang == "en" || lang == "en_us")
		return false;

	return QLocale::system().language() == QLocale::Chinese;
}

static QString trText(const char *zh, const char *en)
{
	return QString::fromUtf8(useChinese() ? zh : en);
}

static QIcon appIcon()
{
	return QIcon(QStringLiteral(":/icons/xiaomi-pen-status.svg"));
}

static QIcon transparentWindowIcon()
{
	QIcon icon;
	for (int size : { 16, 22, 24, 32, 48 }) {
		QPixmap pixmap(size, size);
		pixmap.fill(Qt::transparent);
		icon.addPixmap(pixmap);
	}
	return icon;
}

static bool isDarkMode()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
	const QColor bg = QGuiApplication::palette().window().color();
	return bg.lightness() < 128;
#endif
}

static QString makeStyleSheet(bool dark)
{
	if (!dark) {
		return QStringLiteral(R"(
			QWidget {
				color: #1f2328;
				font-size: 14px;
			}
			#titleLabel {
				font-size: 18px;
				font-weight: 700;
			}
			#stateLabel {
				font-size: 34px;
				font-weight: 800;
			}
			#summaryLabel {
				color: #5f656d;
				font-size: 14px;
			}
			#batteryPanel {
				background: rgba(255, 255, 255, 116);
				border: 1px solid rgba(120, 116, 108, 82);
				border-radius: 8px;
			}
			#captionLabel {
				color: #74736d;
				font-size: 13px;
				font-weight: 600;
			}
			#batteryNumber {
				font-size: 42px;
				font-weight: 800;
			}
			#warningLabel {
				color: #8a4b00;
				font-weight: 700;
			}
			QProgressBar {
				border: 0;
				border-radius: 5px;
				background: rgba(80, 78, 72, 44);
			}
			QProgressBar::chunk {
				border-radius: 5px;
				background: #1f7a5c;
			}
			QGroupBox {
				border: 1px solid rgba(120, 116, 108, 82);
				border-radius: 8px;
				margin-top: 12px;
				padding: 12px;
				background: rgba(255, 255, 255, 92);
			}
			QGroupBox::title {
				subcontrol-origin: margin;
				left: 10px;
				padding: 0 4px;
				color: #65645f;
				font-weight: 600;
			}
			QPushButton {
				border: 1px solid rgba(120, 116, 108, 92);
				border-radius: 6px;
				padding: 6px 12px;
				background: rgba(255, 255, 255, 116);
			}
			QPushButton:hover {
				background: rgba(238, 244, 240, 150);
			}
		)");
	}

	return QStringLiteral(R"(
		* {
			color: #e1e5ea;
		}
		QWidget {
			font-size: 14px;
		}
		#titleLabel {
			font-size: 18px;
			font-weight: 700;
		}
		#stateLabel {
			font-size: 34px;
			font-weight: 800;
		}
		#summaryLabel {
			color: #b4b9c0;
			font-size: 14px;
		}
		#batteryPanel {
			background: rgba(255, 255, 255, 116);
			border: 1px solid rgba(120, 116, 108, 82);
			border-radius: 8px;
		}
		#captionLabel {
			color: #bcc0c5;
			font-size: 13px;
			font-weight: 600;
		}
		#batteryNumber {
			font-size: 42px;
			font-weight: 800;
		}
		#warningLabel {
			color: #f0a040;
			font-weight: 700;
		}
		QProgressBar {
			border: 0;
			border-radius: 5px;
			background: rgba(80, 78, 72, 44);
		}
		QProgressBar::chunk {
			border-radius: 5px;
			background: #1f7a5c;
		}
		QGroupBox {
			border: 1px solid rgba(120, 116, 108, 82);
			border-radius: 8px;
			margin-top: 12px;
			padding: 12px;
			background: rgba(255, 255, 255, 92);
		}
		QGroupBox::title {
			subcontrol-origin: margin;
			left: 10px;
			padding: 0 4px;
			color: #bcc0c5;
			font-weight: 600;
		}
		QPushButton {
			border: 1px solid rgba(120, 116, 108, 92);
			border-radius: 6px;
			padding: 6px 12px;
			background: rgba(255, 255, 255, 116);
			color: #e1e5ea;
		}
		QPushButton:hover {
			background: rgba(238, 244, 240, 150);
		}
	)");
}

struct PenState {
	std::optional<int> hall3;
	std::optional<int> hall4;
	std::optional<int> soc;
	std::optional<int> placeErr;
	std::optional<int> txSs;
	std::optional<int> txIout;
	std::optional<int> txVout;
	std::optional<quint32> macLow;
	std::optional<quint32> macHigh;

	bool valid() const
	{
		return hall3.has_value() || hall4.has_value() || soc.has_value();
	}

	bool placed() const
	{
		return hall3.value_or(1) == 0 || hall4.value_or(1) == 0;
	}

	bool misplaced() const
	{
		return placeErr.value_or(0) != 0;
	}

	bool batteryKnown() const
	{
		return soc.has_value() && *soc >= 0 && *soc <= 100;
	}

	std::optional<QString> macAddress() const
	{
		if (!macLow || !macHigh || (*macLow == 0 && *macHigh == 0))
			return std::nullopt;
		const QString hex = QStringLiteral("%1%2")
			.arg(*macHigh & 0xffff, 4, 16, QLatin1Char('0'))
			.arg(*macLow, 8, 16, QLatin1Char('0'))
			.toUpper();
		QStringList octets;
		for (int offset = 0; offset < hex.size(); offset += 2)
			octets.append(hex.mid(offset, 2));
		return octets.join(QLatin1Char(':'));
	}
};

enum class VisualState {
	Unknown,
	Detached,
	Placed,
	Misplaced,
};

static QIcon makeStatusIcon(VisualState state)
{
	QColor color;
	switch (state) {
	case VisualState::Placed:
		color = QColor(QStringLiteral("#1f7a5c"));
		break;
	case VisualState::Detached:
		color = QColor(QStringLiteral("#3867a6"));
		break;
	case VisualState::Misplaced:
		color = QColor(QStringLiteral("#c66a00"));
		break;
	case VisualState::Unknown:
	default:
		color = QColor(QStringLiteral("#8b8f94"));
		break;
	}

	QIcon icon;
	for (int size : { 16, 22, 24, 32, 48, 64, 128 }) {
		QPixmap pixmap(size, size);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(QRectF(size * 0.0625, size * 0.0625,
					   size * 0.875, size * 0.875));

		QPen pen(Qt::white, qMax(2.0, size * 0.09), Qt::SolidLine, Qt::RoundCap);
		painter.setPen(pen);
		painter.drawLine(QPointF(size * 0.38, size * 0.66),
				 QPointF(size * 0.66, size * 0.34));
		painter.drawPoint(QPointF(size * 0.34, size * 0.70));
		painter.end();

		icon.addPixmap(pixmap);
	}

	return icon;
}

class PenStatusWindow : public QWidget {
public:
	PenStatusWindow()
	{
		const QByteArray envPath = qgetenv("XIAOMI_PEN_SYSFS");
		sysfsBase = envPath.isEmpty() ? QString::fromUtf8(kDefaultSysfs)
					      : QString::fromUtf8(envPath);

		setWindowTitle(trText("手写笔状态", "Stylus Status"));
		setWindowIcon(transparentWindowIcon());
		setFixedSize(500, 600);

		statusDot = new QLabel;
		statusDot->setFixedSize(12, 12);

		titleLabel = new QLabel(trText("手写笔状态", "Stylus Status"));
		titleLabel->setObjectName(QStringLiteral("titleLabel"));

		stateLabel = new QLabel(trText("读取中", "Reading"));
		stateLabel->setObjectName(QStringLiteral("stateLabel"));

		summaryLabel = new QLabel;
		summaryLabel->setObjectName(QStringLiteral("summaryLabel"));
		summaryLabel->setWordWrap(true);

		batteryNumber = new QLabel(QStringLiteral("--"));
		batteryNumber->setObjectName(QStringLiteral("batteryNumber"));

		batteryCaption = new QLabel(trText("电量", "Battery"));
		batteryCaption->setObjectName(QStringLiteral("captionLabel"));

		batteryBar = new QProgressBar;
		batteryBar->setRange(0, 100);
		batteryBar->setTextVisible(false);
		batteryBar->setFixedHeight(10);

		warningLabel = new QLabel;
		warningLabel->setWordWrap(true);
		warningLabel->setObjectName(QStringLiteral("warningLabel"));

		debugGroup = new QGroupBox(trText("调试信息", "Debug"));
		auto *debugLayout = new QGridLayout(debugGroup);
		debugLayout->setColumnStretch(1, 1);
		addDebugRow(debugLayout, 0, QStringLiteral("pen_tx_ss"), &txSsValue);
		addDebugRow(debugLayout, 1, QStringLiteral("tx_iout"), &txIoutValue);
		addDebugRow(debugLayout, 2, QStringLiteral("tx_vout"), &txVoutValue);
		addDebugRow(debugLayout, 3, QStringLiteral("hall3 / hall4"), &hallValue);
		addDebugRow(debugLayout, 4, QStringLiteral("pen_place_err"), &placeErrValue);
		addDebugRow(debugLayout, 5, QStringLiteral("pen_mac"), &macValue);
		addDebugRow(debugLayout, 6, QStringLiteral("refresh_rate"), &refreshRateValue);
		addDebugRow(debugLayout, 7, QStringLiteral("firmware / software"), &versionValue);

		penSettingsGroup = new QGroupBox(
			trText("Focus Pen Pro 设置", "Focus Pen Pro Settings"));
		auto *penSettingsLayout = new QGridLayout(penSettingsGroup);
		penSettingsLayout->setColumnStretch(0, 1);
		pinchLevelLabel = new QLabel;
		pinchLevelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		pinchLevelSlider = new QSlider(Qt::Horizontal);
		pinchLevelSlider->setRange(1, 5);
		pinchLevelSlider->setTickInterval(1);
		pinchLevelSlider->setTickPosition(QSlider::TicksBelow);
		pinchLevelSlider->setValue(settings.value(
			QStringLiteral("focusPenPro/pinchLevel"), 3).toInt());
		penCommandStatus = new QLabel(
			trText("等待 Focus Pen Pro 连接", "Waiting for Focus Pen Pro connection"));
		penCommandStatus->setObjectName(QStringLiteral("summaryLabel"));
		penCommandStatus->setWordWrap(true);
		penSettingsLayout->addWidget(new QLabel(
			trText("轻捏触发力度", "Pinch activation force")), 0, 0);
		penSettingsLayout->addWidget(pinchLevelLabel, 0, 1);
		penSettingsLayout->addWidget(pinchLevelSlider, 1, 0, 1, 2);
		penSettingsLayout->addWidget(penCommandStatus, 2, 0, 1, 2);
		connect(pinchLevelSlider, &QSlider::valueChanged, this,
			[this](int) {
				updatePinchLevelLabel();
				if (!pinchLevelSlider->isSliderDown())
					commitPinchLevel();
			});
		connect(pinchLevelSlider, &QSlider::sliderReleased, this,
			[this]() { commitPinchLevel(); });
		updatePinchLevelLabel();
		penSettingsGroup->setVisible(false);

		auto *refreshButton = new QPushButton(trText("刷新", "Refresh"));
		connect(refreshButton, &QPushButton::clicked, this, &PenStatusWindow::refresh);

		auto *header = new QHBoxLayout;
		header->addWidget(statusDot);
		header->addWidget(titleLabel, 1);
		header->addWidget(refreshButton);

		auto *batteryPanel = new QFrame;
		batteryPanel->setObjectName(QStringLiteral("batteryPanel"));
		auto *batteryPanelLayout = new QVBoxLayout(batteryPanel);
		batteryPanelLayout->setContentsMargins(16, 14, 16, 14);
		batteryPanelLayout->setSpacing(8);
		batteryPanelLayout->addWidget(batteryCaption);
		batteryPanelLayout->addWidget(batteryNumber);
		batteryPanelLayout->addWidget(batteryBar);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(22, 22, 22, 22);
		layout->setSpacing(14);
		layout->addLayout(header);
		layout->addWidget(stateLabel);
		layout->addWidget(summaryLabel);
		layout->addWidget(batteryPanel);
		layout->addWidget(warningLabel);
		layout->addWidget(penSettingsGroup);
		layout->addWidget(debugGroup);

		trayMenu = new QMenu(this);
		showAction = trayMenu->addAction(trText("显示", "Show"));
		quitAction = trayMenu->addAction(trText("退出", "Quit"));
		connect(showAction, &QAction::triggered, this, &PenStatusWindow::showWindow);
		connect(quitAction, &QAction::triggered, qApp, [this]() {
			allowQuit = true;
			qApp->quit();
		});

		tray = new QSystemTrayIcon(this);
		tray->setContextMenu(trayMenu);
		tray->setIcon(makeStatusIcon(VisualState::Unknown));
		tray->setToolTip(trText("手写笔状态", "Stylus Status"));
		connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
			if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
				isVisible() ? hide() : showNormal();
			}
		});
		tray->show();

		timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, &PenStatusWindow::refresh);
		timer->start();

		updateTheme();

		refresh();
	}

	~PenStatusWindow() override
	{
		stopAutoDiscovery();
	}

	void showWindow()
	{
		showNormal();
		raise();
		activateWindow();
	}

protected:
	void closeEvent(QCloseEvent *event) override
	{
		if (allowQuit) {
			event->accept();
			return;
		}

		hide();
		event->ignore();
	}

	void changeEvent(QEvent *event) override
	{
		if (event->type() == QEvent::ApplicationPaletteChange)
			updateTheme();
		QWidget::changeEvent(event);
	}

private:
	static constexpr qint64 kAutoConnectWindowMs = 30000;
	static constexpr qint64 kConnectionGraceMs = 10000;

	void updateTheme()
	{
		const bool dark = isDarkMode();
		setStyleSheet(makeStyleSheet(dark));
	}

	void updatePinchLevelLabel()
	{
		const int level = pinchLevelSlider->value();
		const QString name = [&]() {
			switch (level) {
			case 1: return trText("极弱", "Extreme weak");
			case 2: return trText("较弱", "Weak");
			case 3: return trText("中等", "Medium");
			case 4: return trText("较强", "Strong");
			default: return trText("极强", "Extreme strong");
			}
		}();
		pinchLevelLabel->setText(QStringLiteral("%1 · %2").arg(level).arg(name));
	}

	void commitPinchLevel()
	{
		const int level = pinchLevelSlider->value();
		settings.setValue(QStringLiteral("focusPenPro/pinchLevel"), level);
		appliedPinchLevel = -1;
		nextPenCommandAttempt = 0;
		applyPenSettings();
	}

	bool writePenCommand(const QByteArray &command)
	{
		if (penCommandPath.isEmpty())
			return false;
		QDBusInterface characteristic(QStringLiteral("org.bluez"), penCommandPath,
			QStringLiteral("org.bluez.GattCharacteristic1"),
			QDBusConnection::systemBus());
		QVariantMap options;
		options.insert(QStringLiteral("type"), QStringLiteral("command"));
		const QDBusReply<void> reply = characteristic.call(
			QStringLiteral("WriteValue"), command, options);
		return reply.isValid();
	}

	void applyPenSettings()
	{
		if (penCommandPath.isEmpty() || pinchLevelSlider->isSliderDown())
			return;
		const int level = pinchLevelSlider->value();
		if (appliedPinchLevel == level)
			return;
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (now < nextPenCommandAttempt)
			return;
		if (writePenCommand(focusPenProPinchLevelCommand(level))) {
			appliedPinchLevel = level;
			penCommandStatus->setText(trText(
				"已应用轻捏力度：%1", "Pinch force applied: %1").arg(level));
		} else {
			penCommandStatus->setText(trText(
				"轻捏力度下发失败，将自动重试",
				"Failed to apply pinch force; retrying automatically"));
			nextPenCommandAttempt = now + 3000;
		}
	}

	void addDebugRow(QGridLayout *layout, int row, const QString &name, QLabel **valueLabel)
	{
		auto *nameLabel = new QLabel(name);
		nameLabel->setObjectName(QStringLiteral("captionLabel"));
		*valueLabel = new QLabel(QStringLiteral("-"));
		(*valueLabel)->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(nameLabel, row, 0);
		layout->addWidget(*valueLabel, row, 1);
	}

	PenState readState() const
	{
		PenState state;
		state.hall3 = readInt(sysfsBase + QStringLiteral("/pen_hall3"));
		state.hall4 = readInt(sysfsBase + QStringLiteral("/pen_hall4"));
		state.soc = readInt(sysfsBase + QStringLiteral("/pen_soc"));
		state.placeErr = readInt(sysfsBase + QStringLiteral("/pen_place_err"));
		state.txSs = readInt(sysfsBase + QStringLiteral("/pen_tx_ss"));
		state.txIout = readInt(sysfsBase + QStringLiteral("/tx_iout"));
		state.txVout = readInt(sysfsBase + QStringLiteral("/tx_vout"));
		state.macLow = readHex32(sysfsBase + QStringLiteral("/pen_mac_l"));
		state.macHigh = readHex32(sysfsBase + QStringLiteral("/pen_mac_h"));
		return state;
	}

	void stopAutoDiscovery()
	{
		if (discoveryStartedByUs && !discoveryAdapterPath.isEmpty()) {
			QDBusInterface adapter(QStringLiteral("org.bluez"), discoveryAdapterPath,
					       QStringLiteral("org.bluez.Adapter1"),
					       QDBusConnection::systemBus());
			adapter.call(QDBus::NoBlock, QStringLiteral("StopDiscovery"));
		}
		discoveryStartedByUs = false;
		discoveryAdapterPath.clear();
	}

	void resetAutoConnect()
	{
		stopAutoDiscovery();
		autoConnectMac.clear();
		autoConnectDeadline = 0;
		autoConnectAttemptStarted = 0;
		nextBluetoothAction = 0;
	}

	void serviceAutoConnect(const std::optional<QString> &mac,
				const BluetoothState &state)
	{
		if (!mac) {
			resetAutoConnect();
			return;
		}

		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (autoConnectMac.compare(*mac, Qt::CaseInsensitive) != 0) {
			resetAutoConnect();
			autoConnectMac = *mac;
			autoConnectDeadline = now + kAutoConnectWindowMs;
		}
		if (state.connected) {
			if (state.paired && !state.trusted && !state.devicePath.isEmpty())
				setDeviceTrusted(state.devicePath);
			stopAutoDiscovery();
			return;
		}
		if (now >= autoConnectDeadline) {
			stopAutoDiscovery();
			return;
		}
		if (!state.serviceAvailable || !state.powered || state.adapterPath.isEmpty())
			return;

		if (!state.deviceFound) {
			if (state.discovering && autoConnectAttemptStarted == 0)
				autoConnectAttemptStarted = now;
			if (now < nextBluetoothAction)
				return;
			if (!state.discovering) {
				QDBusInterface adapter(QStringLiteral("org.bluez"), state.adapterPath,
						       QStringLiteral("org.bluez.Adapter1"),
						       QDBusConnection::systemBus());
				QVariantMap filter;
				filter.insert(QStringLiteral("Transport"), QStringLiteral("le"));
				adapter.call(QDBus::NoBlock, QStringLiteral("SetDiscoveryFilter"), filter);
				adapter.call(QDBus::NoBlock, QStringLiteral("StartDiscovery"));
				discoveryStartedByUs = true;
				discoveryAdapterPath = state.adapterPath;
				if (autoConnectAttemptStarted == 0)
					autoConnectAttemptStarted = now;
			}
			nextBluetoothAction = now + 2000;
			return;
		}

		stopAutoDiscovery();
		if (now < nextBluetoothAction || state.devicePath.isEmpty())
			return;
		QDBusInterface device(QStringLiteral("org.bluez"), state.devicePath,
				      QStringLiteral("org.bluez.Device1"),
				      QDBusConnection::systemBus());
		if (!state.paired) {
			device.call(QDBus::NoBlock, QStringLiteral("Pair"));
		} else {
			if (!state.trusted)
				setDeviceTrusted(state.devicePath);
			device.call(QDBus::NoBlock, QStringLiteral("Connect"));
		}
		if (autoConnectAttemptStarted == 0)
			autoConnectAttemptStarted = now;
		nextBluetoothAction = now + 5000;
	}

	static void setDeviceTrusted(const QString &devicePath)
	{
		QDBusInterface properties(QStringLiteral("org.bluez"), devicePath,
					  QStringLiteral("org.freedesktop.DBus.Properties"),
					  QDBusConnection::systemBus());
		properties.call(QDBus::NoBlock, QStringLiteral("Set"),
				QStringLiteral("org.bluez.Device1"),
				QStringLiteral("Trusted"),
				QVariant::fromValue(QDBusVariant(QVariant(true))));
	}

	bool autoConnectPending() const
	{
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		return autoConnectAttemptStarted > 0 && now < autoConnectDeadline &&
		       now - autoConnectAttemptStarted < kConnectionGraceMs;
	}

	void refresh()
	{
		const PenState state = readState();
		const std::optional<QString> mac = state.macAddress();
		const BluetoothState bluetooth = mac ? readBluetoothState(*mac) : BluetoothState{};
		serviceAutoConnect(mac, bluetooth);
		const QString readyAddress = readText(
			QString::fromUtf8(kFocusPenProReadyPath));
		const bool thpReady = mac && readyAddress.compare(
			*mac, Qt::CaseInsensitive) == 0;
		const QString previousCommandPath = penCommandPath;
		const bool settingsReady = bluetooth.connected && bluetooth.servicesResolved &&
			bluetooth.focusPenPro && !bluetooth.commandPath.isEmpty() && thpReady;
		penCommandPath = settingsReady ? bluetooth.commandPath : QString{};
		penSettingsGroup->setVisible(bluetooth.focusPenPro);
		penSettingsGroup->setEnabled(settingsReady);
		setFixedHeight(bluetooth.focusPenPro ? 720 : 600);
		if (penCommandPath != previousCommandPath) {
			appliedPinchLevel = -1;
			nextPenCommandAttempt = 0;
		}
		if (bluetooth.focusPenPro && !bluetooth.connected) {
			penCommandStatus->setText(trText(
				"等待 Focus Pen Pro 蓝牙连接",
				"Waiting for Focus Pen Pro Bluetooth connection"));
		} else if (bluetooth.focusPenPro &&
			   (!bluetooth.servicesResolved || bluetooth.commandPath.isEmpty())) {
			penCommandStatus->setText(trText(
				"等待 Focus Pen Pro 蓝牙服务就绪",
				"Waiting for Focus Pen Pro Bluetooth services"));
		} else if (bluetooth.focusPenPro && !thpReady) {
			penCommandStatus->setText(trText(
				"等待触控服务完成 Focus Pen Pro 初始化",
				"Waiting for the touch service to initialize Focus Pen Pro"));
		}
		applyPenSettings();
		QScreen *screen = QGuiApplication::primaryScreen();
		const qreal refreshRate = screen ? screen->refreshRate() : 0.0;
		const bool rateKnown = refreshRate > 1.0;
		const bool rateSupported = rateKnown &&
			(qAbs(refreshRate - 60.0) < 1.0 || qAbs(refreshRate - 120.0) < 1.0);
		QStringList warnings;

		if (!state.valid()) {
			applyStatus(VisualState::Unknown, QStringLiteral("#9b1c1c"),
				    trText("未找到设备", "Device unavailable"),
				    trText("无法读取手写笔状态", "Unable to read stylus status"));
			connectedNotified = false;
			batteryBar->setValue(0);
			batteryNumber->setText(QStringLiteral("--"));
			warnings.append(trText("请确认 qcom_battmgr 已加载并导出了 Xiaomi 属性。",
					       "Check that qcom_battmgr is loaded and exporting Xiaomi attributes."));
		} else if (state.misplaced()) {
			applyStatus(VisualState::Misplaced, QStringLiteral("#c66a00"),
				    trText("未放好", "Not seated"),
				    trText("请重新放置手写笔", "Reseat the stylus"));
			warnings.append(trText("手写笔没有正确贴合充电位置。",
					       "The stylus is not aligned with the charging position."));
			notifyOnce(trText("手写笔未放好", "Stylus not seated"),
				   trText("请重新放置手写笔。", "Please reseat the stylus."));
			connectedNotified = false;
		} else if (state.placed()) {
			applyStatus(VisualState::Placed, QStringLiteral("#1f7a5c"),
				    trText("已放回", "Docked"),
				    trText("手写笔在充电位置", "Stylus is in the charging position"));
			misplacedNotified = false;
		} else {
			applyStatus(VisualState::Detached, QStringLiteral("#3867a6"),
				    trText("已取下", "Detached"),
				    trText("手写笔未在充电位置", "Stylus is away from the charging position"));
			misplacedNotified = false;
			connectedNotified = false;
		}

		if (state.valid() && state.batteryKnown()) {
			batteryBar->setValue(*state.soc);
			batteryNumber->setText(QStringLiteral("%1%").arg(*state.soc));
			if (state.placed() && !state.misplaced() && !connectedNotified) {
				if (mac && !bluetooth.connected) {
					if (!autoConnectPending()) {
						tray->showMessage(
							trText("手写笔蓝牙未连接", "Stylus Bluetooth disconnected"),
							bluetoothAdvice(*mac, bluetooth),
							QSystemTrayIcon::Warning, 7000);
						connectedNotified = true;
					}
				} else {
					tray->showMessage(trText("手写笔已连接", "Stylus connected"),
							  trText("当前电量 %1%", "Battery %1%").arg(*state.soc),
							  QSystemTrayIcon::Information, 5000);
					connectedNotified = true;
				}
			}
		} else if (state.valid()) {
			batteryBar->setValue(0);
			batteryNumber->setText(QStringLiteral("--"));
		}

		if (mac && !bluetooth.connected) {
			if (autoConnectPending())
				warnings.append(trText("正在尝试自动配对并连接手写笔 %1。",
						       "Trying to pair and connect stylus %1 automatically.")
						.arg(*mac));
			else
				warnings.append(bluetoothAdvice(*mac, bluetooth));
		}
		if (!rateKnown) {
			refreshRateNotified = false;
		} else if (!rateSupported) {
			warnings.append(trText("手写笔仅在 60 Hz 或 120 Hz 刷新率下工作。",
					   "The stylus works only at 60 Hz or 120 Hz."));
			if (!refreshRateNotified && tray->isVisible()) {
				tray->showMessage(
					trText("当前刷新率不支持手写笔", "Refresh rate does not support the stylus"),
					trText("当前为 %1 Hz。请切换到 60 Hz 或 120 Hz。",
					       "The display is at %1 Hz. Switch to 60 Hz or 120 Hz.")
						.arg(refreshRate, 0, 'f', 0),
					QSystemTrayIcon::Warning, 7000);
				refreshRateNotified = true;
			}
		} else {
			refreshRateNotified = false;
		}

		warningLabel->setText(warnings.join(QLatin1Char('\n')));
		updateDebug(state, mac, bluetooth, refreshRate);
	}

	static QString bluetoothAdvice(const QString &mac, const BluetoothState &state)
	{
		if (!state.serviceAvailable || !state.powered)
			return trText("已检测到手写笔 %1，但系统蓝牙未开启。请开启蓝牙并连接此设备。",
				      "Stylus %1 was detected, but Bluetooth is off. Turn it on and connect this device.")
				.arg(mac);
		if (!state.deviceFound)
			return trText("已检测到手写笔 %1，但系统蓝牙中未找到此设备。请配对并连接。",
				      "Stylus %1 was detected, but is not known to BlueZ. Pair and connect it.")
				.arg(mac);
		return trText("已检测到手写笔 %1，但尚未通过蓝牙连接。请在系统蓝牙设置中连接。",
			      "Stylus %1 was detected, but is not connected. Connect it in the system Bluetooth settings.")
			.arg(mac);
	}

	void applyStatus(VisualState visualState, const QString &color, const QString &state,
			 const QString &tooltip)
	{
		statusDot->setStyleSheet(QStringLiteral("border-radius: 6px; background: %1;").arg(color));
		stateLabel->setText(state);
		summaryLabel->setText(tooltip);
		tray->setToolTip(tooltip);
		tray->setIcon(makeStatusIcon(visualState));
	}

	void notifyOnce(const QString &title, const QString &message)
	{
		if (misplacedNotified || !tray->isVisible())
			return;

		tray->showMessage(title, message, QSystemTrayIcon::Warning, 5000);
		misplacedNotified = true;
	}

	void updateDebug(const PenState &state, const std::optional<QString> &mac,
			 const BluetoothState &bluetooth, qreal refreshRate)
	{
		txSsValue->setText(valueText(state.txSs));
		txIoutValue->setText(valueText(state.txIout));
		txVoutValue->setText(valueText(state.txVout));
		hallValue->setText(QStringLiteral("%1 / %2").arg(valueText(state.hall3), valueText(state.hall4)));
		placeErrValue->setText(valueText(state.placeErr));
		macValue->setText(mac.value_or(QStringLiteral("-")));
		refreshRateValue->setText(refreshRate > 1.0
			? QStringLiteral("%1 Hz").arg(refreshRate, 0, 'f', 0)
			: QStringLiteral("-"));
		QStringList versions;
		if (!bluetooth.firmwareRevision.isEmpty())
			versions.append(QStringLiteral("FW %1").arg(bluetooth.firmwareRevision));
		if (!bluetooth.softwareRevision.isEmpty())
			versions.append(QStringLiteral("SW %1").arg(bluetooth.softwareRevision));
		versionValue->setText(versions.isEmpty()
			? QStringLiteral("-") : versions.join(QStringLiteral(" / ")));
	}

	static QString valueText(std::optional<int> value)
	{
		if (!value.has_value())
			return QStringLiteral("-");
		return QString::number(*value);
	}

	QString sysfsBase;
	QLabel *statusDot = nullptr;
	QLabel *titleLabel = nullptr;
	QLabel *stateLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QLabel *batteryNumber = nullptr;
	QLabel *batteryCaption = nullptr;
	QLabel *warningLabel = nullptr;
	QLabel *refreshRateValue = nullptr;
	QLabel *versionValue = nullptr;
	QLabel *macValue = nullptr;
	QLabel *txSsValue = nullptr;
	QLabel *txIoutValue = nullptr;
	QLabel *txVoutValue = nullptr;
	QLabel *hallValue = nullptr;
	QLabel *placeErrValue = nullptr;
	QProgressBar *batteryBar = nullptr;
	QGroupBox *debugGroup = nullptr;
	QGroupBox *penSettingsGroup = nullptr;
	QLabel *pinchLevelLabel = nullptr;
	QLabel *penCommandStatus = nullptr;
	QSlider *pinchLevelSlider = nullptr;
	QSystemTrayIcon *tray = nullptr;
	QMenu *trayMenu = nullptr;
	QAction *showAction = nullptr;
	QAction *quitAction = nullptr;
	QTimer *timer = nullptr;
	bool misplacedNotified = false;
	bool connectedNotified = false;
	bool refreshRateNotified = false;
	bool discoveryStartedByUs = false;
	QString autoConnectMac;
	QString discoveryAdapterPath;
	qint64 autoConnectDeadline = 0;
	qint64 autoConnectAttemptStarted = 0;
	qint64 nextBluetoothAction = 0;
	QSettings settings{QStringLiteral("xiaomi-pen-status"),
			   QStringLiteral("xiaomi-pen-status")};
	QString penCommandPath;
	int appliedPinchLevel = -1;
	qint64 nextPenCommandAttempt = 0;
	bool allowQuit = false;
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("xiaomi-pen-status"));
	app.setDesktopFileName(QStringLiteral("xiaomi-pen-status"));
	app.setWindowIcon(appIcon());
	app.setQuitOnLastWindowClosed(false);

	const QString serverName = QStringLiteral("xiaomi-pen-status");
	QLocalSocket socket;
	socket.connectToServer(serverName);
	if (socket.waitForConnected(100)) {
		socket.write("show");
		socket.waitForBytesWritten(100);
		return 0;
	}

	QLockFile lockFile(QDir::temp().absoluteFilePath(QStringLiteral("xiaomi-pen-status.lock")));
	lockFile.setStaleLockTime(0);
	if (!lockFile.tryLock(100))
		return 0;

	QLocalServer::removeServer(serverName);
	QLocalServer server;
	server.listen(serverName);

	PenStatusWindow window;
	QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window]() {
		while (QLocalSocket *client = server.nextPendingConnection()) {
			client->deleteLater();
		}
		window.showWindow();
	});

	if (app.arguments().contains(QStringLiteral("--show")))
		window.showWindow();

	return app.exec();
}
