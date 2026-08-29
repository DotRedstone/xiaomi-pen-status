#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstring>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
static constexpr const char *kButtonMappingSocket =
	"/run/xiaomi-sheng-thp/button-mapping.sock";

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
				color: #20242a;
				background: #f5f6f8;
				font-size: 14px;
			}
			QLabel {
				background: transparent;
			}
			#titleLabel {
				font-size: 20px;
				font-weight: 700;
			}
			#stateLabel {
				font-size: 28px;
				font-weight: 700;
			}
			#summaryLabel, #mappingStatus {
				color: #626a75;
				font-size: 14px;
			}
			#statusPanel, #mappingPanel, #pinchPanel, #detailsPanel {
				background: #ffffff;
				border: 1px solid #d9dde3;
				border-radius: 8px;
			}
			#sectionTitle {
				font-size: 16px;
				font-weight: 700;
			}
			#captionLabel, #fieldLabel {
				color: #68717d;
				font-size: 13px;
			}
			#batteryNumber {
				font-size: 36px;
				font-weight: 700;
			}
			#warningLabel {
				color: #8a5100;
				background: #fff4df;
				border: 1px solid #efd6a8;
				border-radius: 6px;
				padding: 10px;
			}
			QProgressBar {
				border: 0;
				border-radius: 4px;
				background: #e4e7eb;
			}
			QProgressBar::chunk {
				border-radius: 4px;
				background: #26755c;
			}
			QGroupBox {
				border: 1px solid #d9dde3;
				border-radius: 8px;
				margin-top: 14px;
				padding: 12px;
				background: #ffffff;
			}
			QGroupBox::title {
				subcontrol-origin: margin;
				left: 12px;
				padding: 0 4px;
				color: #4e5661;
				font-weight: 600;
			}
			QComboBox {
				min-height: 40px;
				border: 1px solid #cbd0d8;
				border-radius: 6px;
				padding: 0 12px;
				background: #ffffff;
			}
			QComboBox:hover, QComboBox:focus {
				border-color: #397b68;
			}
			QComboBox QAbstractItemView {
				background: #ffffff;
				selection-background-color: #dcece6;
				selection-color: #20242a;
			}
			QToolButton {
				min-width: 40px;
				min-height: 40px;
				border: 0;
				border-radius: 6px;
				background: transparent;
			}
			QToolButton:hover, QToolButton:pressed {
				background: #e7eaee;
			}
			QTabWidget::pane {
				border: 0;
				background: transparent;
			}
			QTabBar::tab {
				min-width: 108px;
				min-height: 42px;
				padding: 0 14px;
				margin-right: 4px;
				border-radius: 6px;
				background: transparent;
				color: #626a75;
			}
			QTabBar::tab:selected {
				background: #e3eee9;
				color: #1f624f;
				font-weight: 600;
			}
			QScrollArea, QScrollArea > QWidget > QWidget {
				border: 0;
				background: transparent;
			}
		)");
	}

	return QStringLiteral(R"(
		QWidget {
			color: #edf0f3;
			background: #1e2024;
			font-size: 14px;
		}
		QLabel {
			background: transparent;
		}
		#titleLabel {
			font-size: 20px;
			font-weight: 700;
		}
		#stateLabel {
			font-size: 28px;
			font-weight: 700;
		}
		#summaryLabel, #mappingStatus {
			color: #aeb5bf;
			font-size: 14px;
		}
		#statusPanel, #mappingPanel, #pinchPanel, #detailsPanel {
			background: #282b30;
			border: 1px solid #3d424a;
			border-radius: 8px;
		}
		#sectionTitle {
			font-size: 16px;
			font-weight: 700;
		}
		#captionLabel, #fieldLabel {
			color: #aeb5bf;
			font-size: 13px;
		}
		#batteryNumber {
			font-size: 36px;
			font-weight: 700;
		}
		#warningLabel {
			color: #f0bd70;
			background: #3b3021;
			border: 1px solid #685132;
			border-radius: 6px;
			padding: 10px;
		}
		QProgressBar {
			border: 0;
			border-radius: 4px;
			background: #41464e;
		}
		QProgressBar::chunk {
			border-radius: 4px;
			background: #62b99a;
		}
		QGroupBox {
			border: 1px solid #3d424a;
			border-radius: 8px;
			margin-top: 14px;
			padding: 12px;
			background: #282b30;
		}
		QGroupBox::title {
			subcontrol-origin: margin;
			left: 12px;
			padding: 0 4px;
			color: #c8cdd4;
			font-weight: 600;
		}
		QComboBox {
			min-height: 40px;
			border: 1px solid #505660;
			border-radius: 6px;
			padding: 0 12px;
			background: #30343a;
			color: #edf0f3;
		}
		QComboBox:hover, QComboBox:focus {
			border-color: #67a990;
		}
		QComboBox QAbstractItemView {
			background: #30343a;
			selection-background-color: #36594e;
			selection-color: #ffffff;
		}
		QToolButton {
			min-width: 40px;
			min-height: 40px;
			border: 0;
			border-radius: 6px;
			background: transparent;
		}
		QToolButton:hover, QToolButton:pressed {
			background: #353940;
		}
		QTabWidget::pane {
			border: 0;
			background: transparent;
		}
		QTabBar::tab {
			min-width: 108px;
			min-height: 42px;
			padding: 0 14px;
			margin-right: 4px;
			border-radius: 6px;
			background: transparent;
			color: #aeb5bf;
		}
		QTabBar::tab:selected {
			background: #304b43;
			color: #8fd2b8;
			font-weight: 600;
		}
		QScrollArea, QScrollArea > QWidget > QWidget {
			border: 0;
			background: transparent;
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

		setWindowTitle(trText("小米手写笔", "Xiaomi Focus Pen"));
		setWindowIcon(appIcon());
		setMinimumSize(560, 620);
		resize(620, 720);

		statusDot = new QLabel;
		statusDot->setFixedSize(12, 12);

		titleLabel = new QLabel(QStringLiteral("Xiaomi Focus Pen"));
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
		warningLabel->setVisible(false);

		debugGroup = new QGroupBox(trText("硬件数据", "Hardware data"));
		debugGroup->setObjectName(QStringLiteral("detailsPanel"));
		auto *debugLayout = new QGridLayout(debugGroup);
		debugLayout->setContentsMargins(16, 18, 16, 16);
		debugLayout->setHorizontalSpacing(18);
		debugLayout->setVerticalSpacing(12);
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
		penSettingsGroup->setObjectName(QStringLiteral("pinchPanel"));
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

		auto *refreshButton = new QToolButton;
		refreshButton->setIcon(QIcon::fromTheme(
			QStringLiteral("view-refresh-symbolic"),
			style()->standardIcon(QStyle::SP_BrowserReload)));
		refreshButton->setToolTip(trText("刷新", "Refresh"));
		refreshButton->setAccessibleName(refreshButton->toolTip());
		connect(refreshButton, &QToolButton::clicked, this, &PenStatusWindow::refresh);

		auto *header = new QHBoxLayout;
		header->addWidget(titleLabel, 1);
		header->addWidget(refreshButton);

		auto *statusPanel = new QFrame;
		statusPanel->setObjectName(QStringLiteral("statusPanel"));
		auto *statusLayout = new QGridLayout(statusPanel);
		statusLayout->setContentsMargins(18, 18, 18, 18);
		statusLayout->setHorizontalSpacing(12);
		statusLayout->setVerticalSpacing(6);
		statusLayout->addWidget(statusDot, 0, 0, Qt::AlignTop);
		statusLayout->addWidget(stateLabel, 0, 1);
		statusLayout->addWidget(summaryLabel, 1, 1);
		statusLayout->addWidget(batteryCaption, 0, 2, Qt::AlignRight | Qt::AlignBottom);
		statusLayout->addWidget(batteryNumber, 1, 2, Qt::AlignRight | Qt::AlignTop);
		statusLayout->addWidget(batteryBar, 2, 0, 1, 3);
		statusLayout->setColumnStretch(1, 1);

		auto *overviewPage = new QWidget;
		auto *overviewLayout = new QVBoxLayout(overviewPage);
		overviewLayout->setContentsMargins(2, 16, 2, 2);
		overviewLayout->setSpacing(14);
		overviewLayout->addWidget(statusPanel);
		overviewLayout->addWidget(warningLabel);
		overviewLayout->addWidget(penSettingsGroup);
		overviewLayout->addStretch();

		penPrimaryCombo = new QComboBox;
		penSecondaryCombo = new QComboBox;
		airPrimaryCombo = new QComboBox;
		airSecondaryCombo = new QComboBox;
		configureActionCombo(penPrimaryCombo,
			QStringLiteral("buttons/penPrimary"), QStringLiteral("native"));
		configureActionCombo(penSecondaryCombo,
			QStringLiteral("buttons/penSecondary"), QStringLiteral("native"));
		configureActionCombo(airPrimaryCombo,
			QStringLiteral("buttons/airPrimary"), QStringLiteral("left"));
		configureActionCombo(airSecondaryCombo,
			QStringLiteral("buttons/airSecondary"), QStringLiteral("right"));

		auto makeMappingPanel = [this](const QString &title, QComboBox *primary,
						  QComboBox *secondary) {
			auto *panel = new QFrame;
			panel->setObjectName(QStringLiteral("mappingPanel"));
			auto *panelLayout = new QGridLayout(panel);
			panelLayout->setContentsMargins(16, 14, 16, 16);
			panelLayout->setHorizontalSpacing(16);
			panelLayout->setVerticalSpacing(10);
			auto *titleLabel = new QLabel(title);
			titleLabel->setObjectName(QStringLiteral("sectionTitle"));
			auto *primaryLabel = new QLabel(trText("主按键", "Primary button"));
			auto *secondaryLabel = new QLabel(trText("副按键", "Secondary button"));
			primaryLabel->setObjectName(QStringLiteral("fieldLabel"));
			secondaryLabel->setObjectName(QStringLiteral("fieldLabel"));
			panelLayout->addWidget(titleLabel, 0, 0, 1, 2);
			panelLayout->addWidget(primaryLabel, 1, 0);
			panelLayout->addWidget(primary, 1, 1);
			panelLayout->addWidget(secondaryLabel, 2, 0);
			panelLayout->addWidget(secondary, 2, 1);
			panelLayout->setColumnStretch(1, 1);
			return panel;
		};

		mappingStatus = new QLabel;
		mappingStatus->setObjectName(QStringLiteral("mappingStatus"));
		mappingStatus->setWordWrap(true);
		auto *resetButton = new QToolButton;
		resetButton->setIcon(QIcon::fromTheme(
			QStringLiteral("edit-undo-symbolic"),
			style()->standardIcon(QStyle::SP_DialogResetButton)));
		resetButton->setToolTip(trText("恢复默认按键", "Restore default buttons"));
		resetButton->setAccessibleName(resetButton->toolTip());
		connect(resetButton, &QToolButton::clicked, this,
			[this]() { resetButtonMapping(); });

		auto *mappingFooter = new QHBoxLayout;
		mappingFooter->addWidget(mappingStatus, 1);
		mappingFooter->addWidget(resetButton);

		auto *buttonsPage = new QWidget;
		auto *buttonsLayout = new QVBoxLayout(buttonsPage);
		buttonsLayout->setContentsMargins(2, 16, 2, 2);
		buttonsLayout->setSpacing(14);
		buttonsLayout->addWidget(makeMappingPanel(
			trText("书写与悬停", "Writing and hover"),
			penPrimaryCombo, penSecondaryCombo));
		buttonsLayout->addWidget(makeMappingPanel(
			trText("空中指针", "Air pointer"),
			airPrimaryCombo, airSecondaryCombo));
		buttonsLayout->addLayout(mappingFooter);
		buttonsLayout->addStretch();

		auto *detailsPage = new QWidget;
		auto *detailsLayout = new QVBoxLayout(detailsPage);
		detailsLayout->setContentsMargins(2, 16, 2, 2);
		detailsLayout->addWidget(debugGroup);
		detailsLayout->addStretch();

		auto makeScrollPage = [](QWidget *page) {
			auto *scroll = new QScrollArea;
			scroll->setWidgetResizable(true);
			scroll->setFrameShape(QFrame::NoFrame);
			scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scroll->setWidget(page);
			return scroll;
		};
		auto *tabs = new QTabWidget;
		tabs->setDocumentMode(true);
		tabs->addTab(makeScrollPage(overviewPage),
			QIcon::fromTheme(QStringLiteral("input-tablet-symbolic")),
			trText("概览", "Overview"));
		tabs->addTab(makeScrollPage(buttonsPage),
			QIcon::fromTheme(QStringLiteral("preferences-desktop-keyboard-shortcuts-symbolic")),
			trText("按键", "Buttons"));
		tabs->addTab(makeScrollPage(detailsPage),
			QIcon::fromTheme(QStringLiteral("dialog-information-symbolic")),
			trText("设备", "Device"));

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(22, 18, 22, 18);
		layout->setSpacing(8);
		layout->addLayout(header);
		layout->addWidget(tabs, 1);

		trayMenu = new QMenu(this);
		showAction = trayMenu->addAction(trText("打开", "Open"));
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
		if (QSystemTrayIcon::isSystemTrayAvailable())
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
	static constexpr qint64 kButtonMappingRefreshMs = 5000;

	void configureActionCombo(QComboBox *combo, const QString &settingKey,
					  const QString &defaultAction)
	{
		const std::array<std::pair<const char *, QString>, 11> actions{{
			{ "native", trText("默认笔按键", "Default pen button") },
			{ "left", trText("左键单击", "Left click") },
			{ "right", trText("右键单击", "Right click") },
			{ "middle", trText("中键单击", "Middle click") },
			{ "back", trText("返回", "Back") },
			{ "forward", trText("前进", "Forward") },
			{ "undo", trText("撤销", "Undo") },
			{ "redo", trText("重做", "Redo") },
			{ "screenshot", trText("截图", "Screenshot") },
			{ "overview", trText("桌面概览", "Desktop overview") },
			{ "disabled", trText("无操作", "No action") },
		}};
		for (const auto &[value, label] : actions)
			combo->addItem(label, QString::fromLatin1(value));
		const QString saved = settings.value(settingKey, defaultAction).toString();
		int index = combo->findData(saved);
		if (index < 0)
			index = combo->findData(defaultAction);
		combo->setCurrentIndex(index);
		combo->setMinimumWidth(250);
		connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
			[this, combo, settingKey](int) {
				settings.setValue(settingKey, combo->currentData().toString());
				buttonMappingDirty = true;
				applyButtonMapping();
			});
	}

	QString buttonAction(QComboBox *combo) const
	{
		return combo ? combo->currentData().toString() : QStringLiteral("disabled");
	}

	bool sendButtonMapping() const
	{
		const int socketFd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (socketFd < 0)
			return false;
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (std::strlen(kButtonMappingSocket) >= sizeof(address.sun_path)) {
			::close(socketFd);
			return false;
		}
		std::strncpy(address.sun_path, kButtonMappingSocket,
			    sizeof(address.sun_path) - 1);
		const QByteArray payload = QStringLiteral("map %1 %2 %3 %4\n")
			.arg(buttonAction(penPrimaryCombo),
			     buttonAction(penSecondaryCombo),
			     buttonAction(airPrimaryCombo),
			     buttonAction(airSecondaryCombo))
			.toLatin1();
		const ssize_t written = sendto(socketFd, payload.constData(),
			static_cast<size_t>(payload.size()), MSG_NOSIGNAL,
			reinterpret_cast<sockaddr *>(&address), sizeof(address));
		::close(socketFd);
		return written == payload.size();
	}

	void applyButtonMapping()
	{
		if (!mappingStatus)
			return;
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (!buttonMappingDirty &&
		    now - lastButtonMappingAttempt < kButtonMappingRefreshMs)
			return;
		lastButtonMappingAttempt = now;
		if (sendButtonMapping()) {
			buttonMappingDirty = false;
			mappingStatus->setText(trText(
				"按键设置已应用", "Button settings applied"));
		} else {
			buttonMappingDirty = true;
			mappingStatus->setText(trText(
				"等待触控服务", "Waiting for touch service"));
		}
	}

	void resetButtonMapping()
	{
		const std::array<std::pair<QComboBox *, QString>, 4> defaults{{
			{penPrimaryCombo, QStringLiteral("native")},
			{penSecondaryCombo, QStringLiteral("native")},
			{airPrimaryCombo, QStringLiteral("left")},
			{airSecondaryCombo, QStringLiteral("right")},
		}};
		for (const auto &[combo, value] : defaults) {
			const QSignalBlocker blocker(combo);
			combo->setCurrentIndex(combo->findData(value));
		}
		settings.setValue(QStringLiteral("buttons/penPrimary"), QStringLiteral("native"));
		settings.setValue(QStringLiteral("buttons/penSecondary"), QStringLiteral("native"));
		settings.setValue(QStringLiteral("buttons/airPrimary"), QStringLiteral("left"));
		settings.setValue(QStringLiteral("buttons/airSecondary"), QStringLiteral("right"));
		buttonMappingDirty = true;
		applyButtonMapping();
	}

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
		applyButtonMapping();
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
			if (state.placed() && !state.misplaced() && !connectedNotified &&
			    tray->isVisible()) {
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
		warningLabel->setVisible(!warnings.isEmpty());
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
	QLabel *mappingStatus = nullptr;
	QSlider *pinchLevelSlider = nullptr;
	QComboBox *penPrimaryCombo = nullptr;
	QComboBox *penSecondaryCombo = nullptr;
	QComboBox *airPrimaryCombo = nullptr;
	QComboBox *airSecondaryCombo = nullptr;
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
	qint64 lastButtonMappingAttempt = 0;
	bool buttonMappingDirty = true;
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
