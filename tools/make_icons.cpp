// One-off asset generator: renders the inspector's PNG icons (add / remove /
// confirm-delete) into resources/icons/. Run once when the icons need to change;
// the app loads the committed PNGs from the Qt resource system, it never draws
// them at runtime. usage: rpe_makeicons <out-dir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

static QImage canvas(int s)
{
    QImage img(s, s, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    return img;
}

// "+" add icon. Drawn near the canvas edges so it stays legible when scaled down.
static void drawPlus(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.13);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    const qreal m = s * 0.12;
    p.drawLine(QPointF(s / 2.0, m), QPointF(s / 2.0, s - m));
    p.drawLine(QPointF(m, s / 2.0), QPointF(s - m, s / 2.0));
}

// Trash-can remove icon (outline). Fills most of the canvas — no wasted margin.
static void drawTrash(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.085);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const qreal w = s, h = s;
    const qreal lidY = h * 0.22;
    // lid
    p.drawLine(QPointF(w * 0.09, lidY), QPointF(w * 0.91, lidY));
    // handle
    p.drawLine(QPointF(w * 0.37, lidY), QPointF(w * 0.37, h * 0.10));
    p.drawLine(QPointF(w * 0.37, h * 0.10), QPointF(w * 0.63, h * 0.10));
    p.drawLine(QPointF(w * 0.63, h * 0.10), QPointF(w * 0.63, lidY));
    // body (slightly tapered)
    QPainterPath body;
    body.moveTo(w * 0.17, lidY);
    body.lineTo(w * 0.23, h * 0.90);
    body.lineTo(w * 0.77, h * 0.90);
    body.lineTo(w * 0.83, lidY);
    p.drawPath(body);
    // ribs
    p.drawLine(QPointF(w * 0.38, lidY + h * 0.12), QPointF(w * 0.41, h * 0.80));
    p.drawLine(QPointF(w * 0.62, lidY + h * 0.12), QPointF(w * 0.59, h * 0.80));
}

// Check-mark confirm icon (used white, on the warning-coloured button).
static void drawCheck(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.16);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.drawPolyline(QPolygonF({ QPointF(s * 0.14, s * 0.52), QPointF(s * 0.40, s * 0.78), QPointF(s * 0.86, s * 0.22) }));
}

static bool save(const QImage& img, const QString& path)
{
    const bool ok = img.save(path, "PNG");
    printf("%s %s\n", ok ? "wrote" : "FAILED", path.toUtf8().constData());
    return ok;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral(".");

    const int s = 40; // generous so it stays crisp when scaled down on a toolbar
    const QColor ink(0x44, 0x48, 0x4D);
    const QColor white(0xFF, 0xFF, 0xFF);

    bool ok = true;
    {
        QImage img = canvas(s);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawPlus(p, s, ink);
        p.end();
        ok &= save(img, dir + "/add.png");
    }
    {
        QImage img = canvas(s);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawTrash(p, s, ink);
        p.end();
        ok &= save(img, dir + "/remove.png");
    }
    {
        QImage img = canvas(s);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawCheck(p, s, white);
        p.end();
        ok &= save(img, dir + "/confirm.png");
    }
    return ok ? 0 : 1;
}
