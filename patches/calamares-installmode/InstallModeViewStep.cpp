#include "InstallModeViewStep.h"

#include "GlobalStorage.h"
#include "JobQueue.h"
#include "utils/Logger.h"
#include "utils/Variant.h"

#include <QDir>
#include <QFile>

InstallModeViewStep::InstallModeViewStep( QObject* parent )
    : Calamares::QmlViewStep( parent )
    , m_name( nullptr )
{
}

InstallModeViewStep::~InstallModeViewStep() {}

QString
InstallModeViewStep::prettyName() const
{
    return m_name ? m_name->get() : tr( "Install Mode" );
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
    auto* gs = Calamares::JobQueue::instance()->globalStorage();
    QVariant mode = gs->value( QStringLiteral( "installMode" ) );
    if ( mode.isValid() )
    {
        QFile file( QStringLiteral( "/opt/install/install-mode" ) );
        if ( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        {
            file.write( mode.toString().toUtf8() );
            file.write( "\n" );
            file.close();
            cDebug() << "InstallMode: wrote" << mode.toString() << "to /opt/install/install-mode";
        }
        else
        {
            cWarning() << "InstallMode: could not write to /opt/install/install-mode";
        }
    }
    else
    {
        cWarning() << "InstallMode: no installMode in GlobalStorage";
    }
}

void
InstallModeViewStep::setConfigurationMap( const QVariantMap& configurationMap )
{
    bool ok = false;
    auto label = Calamares::getSubMap( configurationMap, QStringLiteral( "qmlLabel" ), ok );
    if ( label.contains( QStringLiteral( "name" ) ) )
    {
        m_name = new Calamares::Locale::TranslatedString( label, QStringLiteral( "name" ) );
    }
    Calamares::QmlViewStep::setConfigurationMap( configurationMap );
}

CALAMARES_PLUGIN_FACTORY_DEFINITION( InstallModeViewStepFactory, registerPlugin< InstallModeViewStep >(); )
