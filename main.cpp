#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QRasterWindow>

class Canvas : public QRasterWindow
{
public:
  Canvas()
  {
    setTitle("Minimal QtGui App");
    resize(400, 300);
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    const QRect r(QPoint(0, 0), size());
    p.fillRect(r, Qt::white);
    p.setPen(Qt::black);
    p.drawText(r, Qt::AlignCenter, "Hello, QtGui!");
  }
};

int main(int argc, char *argv[])
{
  QGuiApplication app(argc, argv);
  Canvas w;
  w.show();
  return app.exec();
}
