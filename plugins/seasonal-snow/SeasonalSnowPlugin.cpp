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
#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginInterface.h"

namespace {

constexpr int FrameIntervalMs = 33;
constexpr int MinimumFlakes = 6;
constexpr int MaximumFlakes = 22;
constexpr int PixelsPerFlake = 52000;
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
    explicit SnowOverlay(QWidget* host)
        : QWidget(host)
        , _host(host)
        , _calendarTimer(this)
        , _frameTimer(this)
    {
        setObjectName(QStringLiteral("seasonalSnowOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(host->rect());

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
        // With WA_NoSystemBackground, explicitly clear the complete surface
        // on every frame: old flake positions must not leave ghost trails.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool dark = palette().color(QPalette::Window).lightness() < 130;
        const QColor frost = dark ? QColor(235, 245, 255, 90)
                                  : QColor(75, 100, 130, 66);
        QPen pen(frost, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);

        for (const Snowflake& flake : _flakes) {
            const double x = flake.baseX
                + flake.sway * std::sin(flake.phase);
            painter.save();
            painter.translate(x, flake.y);
            painter.rotate(flake.angle);
            // Six narrow arms with tiny side branches, slowly rotating.
            for (int arm = 0; arm < 6; ++arm) {
                painter.drawLine(QPointF(0, 0), QPointF(0, -flake.radius));
                const double branchY = -flake.radius * 0.65;
                const double branch = flake.radius * 0.25;
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
            ? randomBetween(-80.0, -flake.radius)
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
        const int desired = qBound(MinimumFlakes,
                                   width() * height() / PixelsPerFlake,
                                   MaximumFlakes);
        while (static_cast<int>(_flakes.size()) < desired) {
            _flakes.push_back(createFlake(false));
        }
        if (static_cast<int>(_flakes.size()) > desired) {
            _flakes.resize(desired);
        }
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
            if (flake.y > height() + flake.radius * 2) {
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
    bool _inSeason = false;
};

} // namespace

class SeasonalSnowPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::MainWindowOverlayInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "SeasonalSnowPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::MainWindowOverlayInterface)

public:
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
        _overlay = new SnowOverlay(window);
    }

private:
    QPointer<SnowOverlay> _overlay;
    bool _enabled = false;
};

#include "SeasonalSnowPlugin.moc"
