#ifndef INSTALLMODEVIEWSTEP_H
#define INSTALLMODEVIEWSTEP_H

#include "DllMacro.h"
#include "locale/TranslatableConfiguration.h"
#include "utils/PluginFactory.h"
#include "viewpages/ViewStep.h"

class QRadioButton;

class PLUGINDLLEXPORT InstallModeViewStep : public Calamares::ViewStep
{
    Q_OBJECT
public:
    explicit InstallModeViewStep( QObject* parent = nullptr );
    ~InstallModeViewStep() override;

    QString prettyName() const override;
    QWidget* widget() override;
    bool isNextEnabled() const override;
    bool isBackEnabled() const override;
    bool isAtBeginning() const override;
    bool isAtEnd() const override;
    Calamares::JobList jobs() const override;

    void onActivate() override;
    void onLeave() override;

    void setConfigurationMap( const QVariantMap& configurationMap ) override;

private:
    QWidget* m_widget;
    QRadioButton* m_offlineBtn;
    QRadioButton* m_onlineBtn;
    Calamares::Locale::TranslatedString* m_name;
};

CALAMARES_PLUGIN_FACTORY_DECLARATION( InstallModeViewStepFactory )

#endif
