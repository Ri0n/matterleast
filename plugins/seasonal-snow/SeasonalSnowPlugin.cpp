#include <algorithm>
#include <cmath>

#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "SnowSeason.h"
#include "SnowSettings.h"
#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginInterface.h"
#include "plugins/api/PluginSettingsInterface.h"

namespace {

constexpr int FrameIntervalMs = 33;
constexpr double Pi = 3.14159265358979323846;

double randomBetween(double lower, double upper)
{
    return lower + (upper - lower) * QRandomGenerator::global()->generateDouble();
}

struct Snowflake {
    double baseX = 0;
    double y = 0;
    double radius = 0;
    double speed = 0;
    double angle = 0;
    double spin = 0;
    double sway = 0;
    double phase = 0;
    double phaseSpeed = 0;
};

class SnowOverlay final : public QWidget
{
public:
    explicit SnowOverlay(QWidget* host, int sizePercent, int intensityPercent)
        : QWidget(host)
        , _host(host)
        , _calendarTimer(this)
        , _frameTimer(this)
        , _sizePercent(sizePercent)
        , _intensityPercent(intensityPercent)
    {
        setObjectName(QStringLiteral("seasonalSnowOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        // This is a child QWidget sharing the main window's backing store,
        // not a translucent top-level window. Let Qt propagate and repaint
        // the content behind us; suppressing the system background would
        // leave transparent clears as black on opaque native windows.
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(host->rect());
        setProperty("snowflakeSizePercent", _sizePercent);

        _calendarTimer.setSingleShot(true);
        connect(&_calendarTimer, &QTimer::timeout, this, [this] {
            refreshSeason();
        });
        _frameTimer.setInterval(FrameIntervalMs);
        _frameTimer.setTimerType(Qt::PreciseTimer);
        connect(&_frameTimer, &QTimer::timeout, this, [this] {
            advance();
        });

        host->installEventFilter(this);
        refreshSeason();
    }

    ~SnowOverlay() override
    {
        _calendarTimer.stop();
        _frameTimer.stop();
        if (_host) {
            _host->removeEventFilter(this);
        }
    }

    void applySettings(int sizePercent, int intensityPercent)
    {
        _sizePercent = sizePercent;
        _intensityPercent = intensityPercent;
        setProperty("snowflakeSizePercent", _sizePercent);
        adjustPopulation();
        update();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != _host || !event) {
            return QWidget::eventFilter(watched, event);
        }

        switch (event->type()) {
        case QEvent::Resize:
            setGeometry(_host->rect());
            adjustPopulation();
            raise();
            break;
        case QEvent::Show:
        case QEvent::WindowStateChange:
            // Changes of visibility and minimize state must not keep an
            // animation timer alive when there is nothing to paint.
            QTimer::singleShot(0, this, [this] { refreshVisibility(); });
            break;
        case QEvent::Hide:
            _frameTimer.stop();
            _clock.invalidate();
            break;
        default:
            break;
        }
        return false;
    }

    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        adjustPopulation();
        refreshVisibility();
    }

    void hideEvent(QHideEvent* event) override
    {
        QWidget::hideEvent(event);
        _frameTimer.stop();
        _clock.invalidate();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        // Do not erase the shared backing store with transparent pixels:
        // Qt composites this child over the other painted widgets. Normal
        // background propagation on each update clears previous snowflakes.
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool dark = palette().color(QPalette::Window).lightness() < 130;
        const QColor frost = dark ? QColor(235, 245, 255, 90)
                                  : QColor(75, 100, 130, 66);
        QPen pen(frost, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);

        for (const Snowflake& flake : _flakes) {
            const double radius = flake.radius
                * MatterLeast::SeasonalSnow::radiusScale(_sizePercent);
            const double x = flake.baseX
                + flake.sway * std::sin(flake.phase);
            painter.save();
            painter.translate(x, flake.y);
            painter.rotate(flake.angle);
            // Six narrow arms with tiny side branches, slowly rotating.
            for (int arm = 0; arm < 6; ++arm) {
                painter.drawLine(QPointF(0, 0), QPointF(0, -radius));
                const double branchY = -radius * 0.65;
                const double branch = radius * 0.25;
                painter.drawLine(QPointF(0, branchY),
                                 QPointF(-branch, branchY - branch));
                painter.drawLine(QPointF(0, branchY),
                                 QPointF(branch, branchY - branch));
                painter.rotate(60);
            }
            painter.restore();
        }
    }

private:
    Snowflake createFlake(bool aboveViewport) const
    {
        Snowflake flake;
        flake.baseX = randomBetween(0, std::max(1, width()));
        flake.radius = randomBetween(3.0, 6.5);
        flake.y = aboveViewport
            ? randomBetween(-80.0, -flake.radius
                * MatterLeast::SeasonalSnow::radiusScale(_sizePercent))
            : randomBetween(0, std::max(1, height()));
        flake.speed = randomBetween(18.0, 34.0); // pixels per second
        flake.angle = randomBetween(0.0, 360.0);
        flake.spin = randomBetween(8.0, 22.0)
            * (randomBetween(0, 1) < 0.5 ? -1.0 : 1.0); // degrees/s
        flake.sway = randomBetween(3.0, 11.0);
        flake.phase = randomBetween(0, 2.0 * Pi);
        flake.phaseSpeed = randomBetween(0.35, 0.9);
        return flake;
    }

    void adjustPopulation()
    {
        if (width() <= 0 || height() <= 0) {
            return;
        }
        const int desired = MatterLeast::SeasonalSnow::flakeCountForArea(
            width(), height(), _intensityPercent);
        while (static_cast<int>(_flakes.size()) < desired) {
            _flakes.push_back(createFlake(false));
        }
        if (static_cast<int>(_flakes.size()) > desired) {
            _flakes.resize(desired);
        }
        setProperty("snowflakeCount", desired);
    }

    void advance()
    {
        const qint64 elapsed = _clock.isValid() ? _clock.restart() : 0;
        if (!_clock.isValid()) {
            _clock.start();
        }
        const double seconds = std::min(0.1, elapsed / 1000.0);
        for (Snowflake& flake : _flakes) {
            flake.y += flake.speed * seconds;
            flake.angle += flake.spin * seconds;
            flake.phase += flake.phaseSpeed * seconds;
            if (flake.y > height() + flake.radius
                * MatterLeast::SeasonalSnow::radiusScale(_sizePercent) * 2) {
                flake = createFlake(true);
            }
        }
        update();
    }

    void refreshSeason()
    {
        // Preview is intentionally opt-in for QA outside the holiday period.
        // It does not affect ordinary users or any persisted preferences.
        _inSeason = MatterLeast::SeasonalSnow::activeOn(QDate::currentDate())
            || qEnvironmentVariable("MATTERLEAST_SNOW_PREVIEW") == QLatin1String("1");
        _calendarTimer.start(MatterLeast::SeasonalSnow::untilNextLocalMidnight(
            QDateTime::currentDateTime()));
        refreshVisibility();
    }

    void refreshVisibility()
    {
        const bool visible = _inSeason && _host && _host->isVisible()
            && !_host->isMinimized();
        if (!visible) {
            if (isVisible()) hide();
            _frameTimer.stop();
            _clock.invalidate();
            return;
        }
        setGeometry(_host->rect());
        adjustPopulation();
        if (!isVisible()) show();
        raise();
        if (!_frameTimer.isActive()) {
            _clock.start();
            _frameTimer.start();
        }
    }

    QPointer<QWidget> _host;
    QTimer _calendarTimer;
    QTimer _frameTimer;
    QElapsedTimer _clock;
    QVector<Snowflake> _flakes;
    int _sizePercent = MatterLeast::SeasonalSnow::DefaultSizePercent;
    int _intensityPercent = MatterLeast::SeasonalSnow::DefaultIntensityPercent;
    bool _inSeason = false;
};

} // namespace

class SeasonalSnowPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::MainWindowOverlayInterface
    , public MatterLeast::PluginApi::PluginSettingsInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "SeasonalSnowPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::MainWindowOverlayInterface)
    Q_INTERFACES(MatterLeast::PluginApi::PluginSettingsInterface)

public:
    QList<QVariantHash> settingDescriptors() const override
    {
        return {
            {
                {QStringLiteral("key"), QStringLiteral("sizePercent")},
                {QStringLiteral("title"), tr("Snowflake size")},
                {QStringLiteral("description"), tr("Relative to the original snowflake size.")},
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("default"), MatterLeast::SeasonalSnow::DefaultSizePercent},
                {QStringLiteral("minimum"), 50},
                {QStringLiteral("maximum"), 300},
                {QStringLiteral("step"), 10},
                {QStringLiteral("suffix"), QStringLiteral("%")}
            },
            {
                {QStringLiteral("key"), QStringLiteral("intensityPercent")},
                {QStringLiteral("title"), tr("Snow intensity")},
                {QStringLiteral("description"), tr("Number of falling snowflakes relative to the original density.")},
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("default"), MatterLeast::SeasonalSnow::DefaultIntensityPercent},
                {QStringLiteral("minimum"), 25},
                {QStringLiteral("maximum"), 250},
                {QStringLiteral("step"), 25},
                {QStringLiteral("suffix"), QStringLiteral("%")}
            }
        };
    }

    bool setSettingValue(const QString& key, const QVariant& value) override
    {
        if (value.userType() != QMetaType::Int) {
            return false;
        }
        const int percent = value.toInt();
        if (key == QLatin1String("sizePercent")) {
            if (percent < 50 || percent > 300) return false;
            _sizePercent = percent;
        } else if (key == QLatin1String("intensityPercent")) {
            if (percent < 25 || percent > 250) return false;
            _intensityPercent = percent;
        } else {
            return false;
        }
        if (_overlay) {
            _overlay->applySettings(_sizePercent, _intensityPercent);
        }
        return true;
    }

    bool enable() override
    {
        _enabled = true;
        return true;
    }

    bool disable() override
    {
        // Do not unload a plugin library while one of its QWidget subclasses
        // or event filters still belongs to the application's widget tree.
        delete _overlay.data();
        _overlay.clear();
        _enabled = false;
        return true;
    }

    void attachToMainWindow(QWidget* window) override
    {
        if (!_enabled || !window) {
            return;
        }
        if (_overlay && _overlay->parentWidget() == window) {
            return;
        }
        delete _overlay.data();
        _overlay = new SnowOverlay(window, _sizePercent, _intensityPercent);
    }

private:
    QPointer<SnowOverlay> _overlay;
    int _sizePercent = MatterLeast::SeasonalSnow::DefaultSizePercent;
    int _intensityPercent = MatterLeast::SeasonalSnow::DefaultIntensityPercent;
    bool _enabled = false;
};

#include "SeasonalSnowPlugin.moc"
