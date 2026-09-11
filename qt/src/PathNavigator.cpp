#include "PathNavigator.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QStyleOptionToolButton>

PathCrumbButton::PathCrumbButton(const QString &label, const QString &path, bool current,
                                 QWidget *parent)
    : QPushButton(label, parent), m_path(path), m_current(current) {
  setFlat(true);
  setCursor(Qt::ArrowCursor);
  setFocusPolicy(Qt::TabFocus);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  setMouseTracking(true);
  setAutoFillBackground(false);
  setAttribute(Qt::WA_Hover, true);
  setAttribute(Qt::WA_LayoutUsesWidgetRect, true);
  setAttribute(Qt::WA_StyledBackground, false);
  setAutoDefault(false);
  setDefault(false);
}

QPoint PathCrumbButton::arrowMenuPos() const {
  const int popupX = width() - arrowWidth();
  return mapToGlobal(QPoint(popupX, height()));
}

int PathCrumbButton::arrowWidth() const {
  if (m_current)
    return 0;
  const int h = height() > 0 ? height() : fontMetrics().height() + 6;
  return qMax(h / 2, 4);
}

bool PathCrumbButton::isAboveArrow(int x) const {
  const int aw = arrowWidth();
  if (aw <= 0)
    return false;
  return x >= width() - aw;
}

QSize PathCrumbButton::sizeHint() const {
  QFont f(font());
  f.setBold(m_current);
  const int textW = QFontMetrics(f).horizontalAdvance(text());
  const int h = qMax(QPushButton::sizeHint().height(), fontMetrics().height() + 6);
  const int aw = m_current ? 0 : qMax(h / 2, 4);
  return QSize(kPadding + textW + aw + kPadding, h);
}

QSize PathCrumbButton::minimumSizeHint() const {
  QSize hint = sizeHint();
  if (hint.width() > 150)
    hint.setWidth(150);
  return hint;
}

void PathCrumbButton::drawHoverBackground(QPainter *painter) {
  if (!m_hover && !hasFocus())
    return;
  QStyleOptionToolButton option;
  option.initFrom(this);
  option.rect = rect().adjusted(0, 1, 0, -1);
  option.state |= QStyle::State_AutoRaise | QStyle::State_MouseOver | QStyle::State_Raised;
  option.subControls = QStyle::SC_ToolButton;
  option.activeSubControls = QStyle::SC_ToolButton;
  option.toolButtonStyle = Qt::ToolButtonTextOnly;
  style()->drawPrimitive(QStyle::PE_PanelButtonTool, &option, painter, this);
}

QColor PathCrumbButton::foregroundColor() const {
  return palette().color(QPalette::Active, QPalette::WindowText);
}

void PathCrumbButton::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  QFont adjusted(font());
  adjusted.setBold(m_current);
  painter.setFont(adjusted);

  const int aw = arrowWidth();
  const int h = height();
  int buttonWidth = width();
  const int preferred = sizeHint().width();
  if (buttonWidth > preferred)
    buttonWidth = preferred;

  const QRect textRect(kPadding, 0, buttonWidth - aw - kPadding, h);
  drawHoverBackground(&painter);

  painter.setPen(foregroundColor());
  const QString elided = QFontMetrics(adjusted).elidedText(text(), Qt::ElideRight, textRect.width());
  painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
  if (elided != text())
    setToolTip(text());
  else
    setToolTip(QString());

  if (aw > 0) {
    QStyleOption option;
    option.initFrom(this);
    option.palette = palette();
    const QColor fg = foregroundColor();
    option.palette.setColor(QPalette::Text, fg);
    option.palette.setColor(QPalette::WindowText, fg);
    option.palette.setColor(QPalette::ButtonText, fg);
    option.rect = QRect(textRect.right(), 0, aw, h);
    if (!m_hoverArrow)
      option.state = QStyle::State_None;
    style()->drawPrimitive(QStyle::PE_IndicatorArrowRight, &option, &painter, this);
  }
}

void PathCrumbButton::enterEvent(QEnterEvent *event) {
  QPushButton::enterEvent(event);
  m_hover = true;
  update();
}

void PathCrumbButton::leaveEvent(QEvent *event) {
  QPushButton::leaveEvent(event);
  m_hover = false;
  m_hoverArrow = false;
  update();
}

void PathCrumbButton::mouseMoveEvent(QMouseEvent *event) {
  QPushButton::mouseMoveEvent(event);
  const bool overArrow = isAboveArrow(qRound(event->position().x()));
  if (overArrow != m_hoverArrow) {
    m_hoverArrow = overArrow;
    update();
  }
}

void PathCrumbButton::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    m_pressedOnArrow = isAboveArrow(qRound(event->position().x()));
    setDown(true);
    update();
  }
  event->accept();
}

void PathCrumbButton::mouseReleaseEvent(QMouseEvent *event) {
  const bool onArrow = isAboveArrow(qRound(event->position().x()));
  const bool inside = rect().contains(event->position().toPoint());
  // Swallow QPushButton::clicked so the cursor/style never treats crumbs as links.
  setDown(false);
  update();
  if (event->button() != Qt::LeftButton || !inside)
    return;
  if (m_pressedOnArrow && onArrow)
    emit arrowClicked();
  else if (!m_pressedOnArrow && !onArrow)
    emit activated();
}

PathNavigator::PathNavigator(QWidget *parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  setCursor(Qt::ArrowCursor);
  setAutoFillBackground(false);
  setAttribute(Qt::WA_LayoutUsesWidgetRect, true);
  setAttribute(Qt::WA_Hover, true);
  setAttribute(Qt::WA_StyledBackground, false);
  setAttribute(Qt::WA_NoSystemBackground, true);

  m_layout = new QHBoxLayout(this);
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(0);
  m_layout->addStretch();
}

QSize PathNavigator::sizeHint() const {
  const int h = qMax(fontMetrics().height() + 12, 28);
  return QSize(200, h);
}

QSize PathNavigator::minimumSizeHint() const {
  return QSize(80, sizeHint().height());
}

void PathNavigator::setPath(const QString &path) {
  QString normalized = path;
  if (normalized == QLatin1String("/"))
    normalized.clear();
  if (normalized == m_path && m_layout->count() > 1)
    return;
  m_path = normalized;
  rebuild();
}

void PathNavigator::rebuild() {
  while (QLayoutItem *item = m_layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }

  QStringList parts = m_path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  parts.prepend(QString());

  for (int i = 0; i < parts.size(); ++i) {
    const bool current = (i + 1 == parts.size());
    const QString label =
        parts[i].isEmpty() ? QStringLiteral("My Documents") : parts[i];
    const QString target = QStringList(parts.mid(0, i + 1)).join(QLatin1Char('/'));
    auto *btn = new PathCrumbButton(label, target, current, this);
    connect(btn, &PathCrumbButton::activated, this, [this, target] { emit pathActivated(target); });
    connect(btn, &PathCrumbButton::arrowClicked, this, [this, btn] {
      emit subdirsRequested(btn->path(), btn->arrowMenuPos());
    });
    m_layout->addWidget(btn);
  }
  m_layout->addStretch();
}

void PathNavigator::paintEvent(QPaintEvent *) {
  // No fill or frame — sit on the window background so blur/themes pass through.
}
