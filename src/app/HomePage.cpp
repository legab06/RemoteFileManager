#include "remotefilemanager/app/HomePage.hpp"

#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace rfm::app
{
namespace
{

class HomeContent final : public QWidget
{
  public:
    explicit HomeContent(QWidget* parent = nullptr) : QWidget(parent) {}

    void setPreferredWidth(int width)
    {
        if (m_preferredWidth == width) {
            return;
        }
        m_preferredWidth = width;
        updateGeometry();
    }

    QSize sizeHint() const override
    {
        QSize size = QWidget::sizeHint();
        size.setWidth(qMax(size.width(), m_preferredWidth));
        return size;
    }

  private:
    int m_preferredWidth{0};
};

class HomeActionsLayout final : public QHBoxLayout
{
  public:
    int textMinimumWidth() const { return QHBoxLayout::minimumSize().width(); }

    QSize minimumSize() const override
    {
        QSize size = QHBoxLayout::minimumSize();
        size.setWidth(0);
        return size;
    }
};

} // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("homePage"));

    auto* const outerLayout = new QVBoxLayout(this);
    outerLayout->setSizeConstraint(QLayout::SetMinimumSize);
    outerLayout->setContentsMargins(48, 36, 48, 36);
    outerLayout->addStretch();

    auto* const content = new HomeContent(this);
    m_content = content;
    content->setMaximumWidth(720);
    auto* const layout = new QVBoxLayout(content);
    layout->setSpacing(12);

    auto* const title = new QLabel(tr("RemoteFileManager"), content);
    title->setObjectName(QStringLiteral("homeTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto* const description =
        new QLabel(tr("Connect to a saved server or start a new secure SSH connection."), content);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* const serversLabel = new QLabel(tr("Servers"), content);
    QFont serversFont = serversLabel->font();
    serversFont.setBold(true);
    serversLabel->setFont(serversFont);
    layout->addWidget(serversLabel);

    m_serverList = new QListWidget(content);
    m_serverList->setObjectName(QStringLiteral("homeServerList"));
    m_serverList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_serverList->setMinimumHeight(170);
    layout->addWidget(m_serverList);

    auto* const buttons = new HomeActionsLayout;
    m_actionsLayout = buttons;
    buttons->setDirection(QBoxLayout::LeftToRight);
    buttons->setSizeConstraint(QLayout::SetNoConstraint);
    m_connectButton = new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton),
                                      tr("Connect"), content);
    m_connectButton->setObjectName(QStringLiteral("homeConnectButton"));
    m_connectButton->setIconSize(QSize(24, 24));
    m_connectButton->setToolTip(tr("Connect"));
    m_connectButton->setAccessibleName(tr("Connect"));
    m_connectButton->setEnabled(false);
    m_editButton = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                                   tr("Edit…"), content);
    m_editButton->setObjectName(QStringLiteral("homeEditServerButton"));
    m_editButton->setIconSize(QSize(24, 24));
    m_editButton->setToolTip(tr("Edit server"));
    m_editButton->setAccessibleName(tr("Edit server"));
    m_editButton->setEnabled(false);
    m_newConnectionButton = new QPushButton(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), content);
    m_newConnectionButton->setObjectName(QStringLiteral("homeNewConnectionButton"));
    m_newConnectionButton->setIconSize(QSize(24, 24));
    m_newConnectionButton->setToolTip(tr("New connection"));
    m_newConnectionButton->setAccessibleName(tr("New connection"));
    buttons->addWidget(m_connectButton);
    buttons->addWidget(m_editButton);
    buttons->addStretch();
    buttons->addWidget(m_newConnectionButton);
    layout->addLayout(buttons);

    outerLayout->addWidget(content, 0, Qt::AlignHCenter);
    outerLayout->addStretch();

    m_compactContentWidth = content->sizeHint().width();
    updateContentWidth();
    updateActionPresentation();
    content->layout()->activate();
    setMinimumHeight(minimumSizeHint().height());

    connect(m_serverList, &QListWidget::itemSelectionChanged, this, &HomePage::updateConnectButton);
    connect(m_serverList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { requestSelectedProfile(); });
    connect(m_connectButton, &QPushButton::clicked, this, &HomePage::requestSelectedProfile);
    connect(m_editButton, &QPushButton::clicked, this, [this] {
        if (m_serverList->currentItem() != nullptr && m_editButton->isEnabled()) {
            emit editProfileRequested(m_serverList->currentItem()->data(Qt::UserRole).toString());
        }
    });
    connect(m_newConnectionButton, &QPushButton::clicked, this, &HomePage::newConnectionRequested);
}

QSize HomePage::minimumSizeHint() const
{
    QSize size = QWidget::minimumSizeHint();
    if (m_content == nullptr) {
        return size;
    }
    const auto* const contentLayout = m_content->layout();
    const int contentHeight = qMax(contentLayout->sizeHint().height(),
                                   contentLayout->heightForWidth(m_compactContentWidth));
    const QMargins outerMargins = layout()->contentsMargins();
    size.setHeight(
        qMax(size.height(), contentHeight + outerMargins.top() + outerMargins.bottom()));
    return size;
}

void HomePage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateContentWidth();
    updateActionPresentation();
    setMinimumHeight(qMax(minimumHeight(), minimumSizeHint().height()));
}

void HomePage::updateActionPresentation()
{
    m_content->layout()->activate();
    const QString connectText = tr("Connect");
    const QString editText = tr("Edit…");
    const QString newConnectionText = tr("New connection…");
    m_connectButton->setText(connectText);
    m_editButton->setText(editText);
    m_newConnectionButton->setText(newConnectionText);
    m_actionsLayout->activate();
    const auto* const actions = static_cast<HomeActionsLayout*>(m_actionsLayout);
    const bool showText =
        actions->textMinimumWidth() <= m_actionsLayout->geometry().width();
    if (!showText) {
        m_connectButton->setText({});
        m_editButton->setText({});
        m_newConnectionButton->setText({});
    }
    updateGeometry();
}

void HomePage::updateContentWidth()
{
    constexpr int expansionStartWidth = 1000;
    constexpr int maximumContentWidth = 720;
    auto* const content = static_cast<HomeContent*>(m_content);
    const int extraWidth = qMax(0, width() - expansionStartWidth) / 2;
    content->setPreferredWidth(qMin(m_compactContentWidth + extraWidth, maximumContentWidth));
}

void HomePage::setProfiles(const QList<rfm::core::ConnectionProfile>& profiles)
{
    const QString selectedId = m_serverList->currentItem() == nullptr
                                   ? QString{}
                                   : m_serverList->currentItem()->data(Qt::UserRole).toString();
    m_serverList->clear();
    if (profiles.isEmpty()) {
        auto* const emptyItem = new QListWidgetItem(tr("No saved servers yet"), m_serverList);
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
        updateConnectButton();
        return;
    }

    for (const rfm::core::ConnectionProfile& profile : profiles) {
        auto* const item =
            new QListWidgetItem(style()->standardIcon(QStyle::SP_ComputerIcon),
                                tr("%1\n%2@%3:%4")
                                    .arg(profile.effectiveDisplayName(), profile.username,
                                         profile.host, QString::number(profile.port)),
                                m_serverList);
        item->setData(Qt::UserRole, profile.id);
        if (profile.id == selectedId) {
            m_serverList->setCurrentItem(item);
        }
    }
    updateConnectButton();
}

void HomePage::updateConnectButton()
{
    m_connectButton->setEnabled(
        m_serverList->currentItem() != nullptr &&
        !m_serverList->currentItem()->data(Qt::UserRole).toString().trimmed().isEmpty());
    m_editButton->setEnabled(m_connectButton->isEnabled());
}

void HomePage::requestSelectedProfile()
{
    if (!m_connectButton->isEnabled() || m_serverList->currentItem() == nullptr) {
        return;
    }
    emit connectProfileRequested(m_serverList->currentItem()->data(Qt::UserRole).toString());
}

} // namespace rfm::app
