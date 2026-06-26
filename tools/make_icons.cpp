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

// "+" add icon.
static void drawPlus(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.11);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    const qreal m = s * 0.24;
    p.drawLine(QPointF(s / 2.0, m), QPointF(s / 2.0, s - m));
    p.drawLine(QPointF(m, s / 2.0), QPointF(s - m, s / 2.0));
}

// Trash-can remove icon (outline).
static void drawTrash(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.075);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const qreal w = s, h = s;
    const qreal lidY = h * 0.28;
    // lid
    p.drawLine(QPointF(w * 0.18, lidY), QPointF(w * 0.82, lidY));
    // handle
    p.drawLine(QPointF(w * 0.40, lidY), QPointF(w * 0.40, h * 0.18));
    p.drawLine(QPointF(w * 0.40, h * 0.18), QPointF(w * 0.60, h * 0.18));
    p.drawLine(QPointF(w * 0.60, h * 0.18), QPointF(w * 0.60, lidY));
    // body (slightly tapered)
    QPainterPath body;
    body.moveTo(w * 0.26, lidY);
    body.lineTo(w * 0.30, h * 0.82);
    body.lineTo(w * 0.70, h * 0.82);
    body.lineTo(w * 0.74, lidY);
    p.drawPath(body);
    // ribs
    p.drawLine(QPointF(w * 0.42, lidY + h * 0.10), QPointF(w * 0.44, h * 0.74));
    p.drawLine(QPointF(w * 0.58, lidY + h * 0.10), QPointF(w * 0.56, h * 0.74));
}

// Check-mark confirm icon (used white, on the warning-coloured button).
static void drawCheck(QPainter& p, int s, const QColor& c)
{
    QPen pen(c);
    pen.setWidthF(s * 0.13);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.drawPolyline(QPolygonF({ QPointF(s * 0.22, s * 0.52), QPointF(s * 0.42, s * 0.72), QPointF(s * 0.78, s * 0.30) }));
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
