#pragma once

#include <QPushButton>
#include <QWidget>

class QHBoxLayout;

class PathCrumbButton : public QPushButton {
  Q_OBJECT
public:
  PathCrumbButton(const QString &label, const QString &path, bool current, QWidget *parent = nullptr);

  QString path() const { return m_path; }
  QPoint arrowMenuPos() const;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void activated();
  void arrowClicked();

protected:
  void paintEvent(QPaintEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  int arrowWidth() const;
  bool isAboveArrow(int x) const;
  void drawHoverBackground(QPainter *painter);
  QColor foregroundColor() const;

  QString m_path;
  bool m_current = false;
  bool m_hover = false;
  bool m_hoverArrow = false;
  bool m_pressedOnArrow = false;
  static constexpr int kPadding = 5;
};

class PathNavigator : public QWidget {
  Q_OBJECT
public:
  explicit PathNavigator(QWidget *parent = nullptr);

  void setPath(const QString &path);
  QString path() const { return m_path; }

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void pathActivated(const QString &path);
  void subdirsRequested(const QString &path, const QPoint &globalPos);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  void rebuild();

  QString m_path;
  QHBoxLayout *m_layout = nullptr;
};
