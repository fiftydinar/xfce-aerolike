#ifndef INSTALLMODEVIEWSTEP_H
#define INSTALLMODEVIEWSTEP_H

#include "DllMacro.h"
#include "locale/TranslatableConfiguration.h"
#include "utils/PluginFactory.h"
#include "viewpages/QmlViewStep.h"

class PLUGINDLLEXPORT InstallModeViewStep : public Calamares::QmlViewStep
{
    Q_OBJECT
public:
    explicit InstallModeViewStep( QObject* parent = nullptr );
    ~InstallModeViewStep() override;

    QString prettyName() const override;
    bool isNextEnabled() const override;
    bool isBackEnabled() const override;
    bool isAtBeginning() const override;
    bool isAtEnd() const override;
    Calamares::JobList jobs() const override;

    void onActivate() override;
    void onLeave() override;

    void setConfigurationMap( const QVariantMap& configurationMap ) override;

private:
    Calamares::Locale::TranslatedString* m_name;
};

CALAMARES_PLUGIN_FACTORY_DECLARATION( InstallModeViewStepFactory )

#endif
