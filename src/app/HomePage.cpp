#include "remotefilemanager/app/HomePage.hpp"

#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace rfm::app
{

HomePage::HomePage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("homePage"));

    auto* const outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(48, 36, 48, 36);
    outerLayout->addStretch();

    auto* const content = new QWidget(this);
    content->setMaximumWidth(620);
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

    auto* const buttons = new QHBoxLayout;
    m_connectButton = new QPushButton(tr("Connect"), content);
    m_connectButton->setObjectName(QStringLiteral("homeConnectButton"));
    m_connectButton->setEnabled(false);
    m_editButton = new QPushButton(tr("Edit…"), content);
    m_editButton->setObjectName(QStringLiteral("homeEditServerButton"));
    m_editButton->setEnabled(false);
    auto* const newConnectionButton = new QPushButton(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), content);
    newConnectionButton->setObjectName(QStringLiteral("homeNewConnectionButton"));
    buttons->addWidget(m_connectButton);
    buttons->addWidget(m_editButton);
    buttons->addStretch();
    buttons->addWidget(newConnectionButton);
    layout->addLayout(buttons);

    outerLayout->addWidget(content, 0, Qt::AlignHCenter);
    outerLayout->addStretch();

    connect(m_serverList, &QListWidget::itemSelectionChanged, this, &HomePage::updateConnectButton);
    connect(m_serverList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { requestSelectedProfile(); });
    connect(m_connectButton, &QPushButton::clicked, this, &HomePage::requestSelectedProfile);
    connect(m_editButton, &QPushButton::clicked, this, [this] {
        if (m_serverList->currentItem() != nullptr && m_editButton->isEnabled()) {
            emit editProfileRequested(m_serverList->currentItem()->data(Qt::UserRole).toString());
        }
    });
    connect(newConnectionButton, &QPushButton::clicked, this, &HomePage::newConnectionRequested);
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
