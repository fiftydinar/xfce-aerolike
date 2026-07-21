#include "InstallModeViewStep.h"

#include "GlobalStorage.h"
#include "JobQueue.h"
#include "utils/Logger.h"
#include "utils/Variant.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

InstallModeViewStep::InstallModeViewStep( QObject* parent )
    : Calamares::ViewStep( parent )
    , m_widget( nullptr )
    , m_offlineBtn( nullptr )
    , m_onlineBtn( nullptr )
    , m_name( nullptr )
{
}

InstallModeViewStep::~InstallModeViewStep()
{
    if ( m_widget )
    {
        m_widget->deleteLater();
    }
}

QString
InstallModeViewStep::prettyName() const
{
    return m_name ? m_name->get() : tr( "Install Mode" );
}

QWidget*
InstallModeViewStep::widget()
{
    if ( !m_widget )
    {
        m_widget = new QWidget();

        auto* layout = new QVBoxLayout( m_widget );
        layout->setContentsMargins( 20, 20, 20, 20 );
        layout->setSpacing( 12 );

        auto* title = new QLabel( tr( "<b>Select install mode</b>" ) );
        layout->addWidget( title );

        auto* desc = new QLabel(
            tr( "Choose how the system image is sourced for installation:" ) );
        desc->setWordWrap( true );
        layout->addWidget( desc );

        m_offlineBtn = new QRadioButton(
            tr( "Offline - use image bundled in the ISO (no network required)" ) );
        m_offlineBtn->setChecked( true );
        layout->addWidget( m_offlineBtn );

        m_onlineBtn = new QRadioButton(
            tr( "Online - pull the latest image from the registry" ) );
        layout->addWidget( m_onlineBtn );

        layout->addStretch();
    }
    return m_widget;
}

bool
InstallModeViewStep::isNextEnabled() const
{
    return true;
}

bool
InstallModeViewStep::isBackEnabled() const
{
    return true;
}

bool
InstallModeViewStep::isAtBeginning() const
{
    return true;
}

bool
InstallModeViewStep::isAtEnd() const
{
    return true;
}

Calamares::JobList
InstallModeViewStep::jobs() const
{
    return Calamares::JobList();
}

void
InstallModeViewStep::onActivate()
{
}

void
InstallModeViewStep::onLeave()
{
    QString mode = m_onlineBtn && m_onlineBtn->isChecked()
        ? QStringLiteral( "online" )
        : QStringLiteral( "offline" );

    auto* gs = Calamares::JobQueue::instance()->globalStorage();
    gs->insert( QStringLiteral( "installMode" ), mode );

    QFile file( QStringLiteral( "/opt/install/install-mode" ) );
    if ( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        file.write( mode.toUtf8() );
        file.write( "\n" );
        file.close();
        cDebug() << "InstallMode:" << mode;
    }
    else
    {
        cWarning() << "InstallMode: could not write to /opt/install/install-mode";
    }
}

void
InstallModeViewStep::setConfigurationMap( const QVariantMap& configurationMap )
{
    if ( configurationMap.contains( QStringLiteral( "label" ) ) )
    {
        auto label = configurationMap.value( QStringLiteral( "label" ) ).toMap();
        if ( label.contains( QStringLiteral( "name" ) ) )
        {
            m_name = new Calamares::Locale::TranslatedString( label, QStringLiteral( "name" ) );
        }
    }
    else if ( configurationMap.contains( QStringLiteral( "name" ) ) )
    {
        m_name = new Calamares::Locale::TranslatedString(
            configurationMap.value( QStringLiteral( "name" ) ).toString() );
    }
}

CALAMARES_PLUGIN_FACTORY_DEFINITION( InstallModeViewStepFactory, registerPlugin< InstallModeViewStep >(); )
