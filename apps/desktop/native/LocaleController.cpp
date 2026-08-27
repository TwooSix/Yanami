#include "LocaleController.hpp"

#include <QCoreApplication>
#include <QQmlEngine>
#include <QSettings>

LocaleController::LocaleController(QQmlEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    const QString savedLanguage = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("en")).toString();
    applyLanguage(savedLanguage, false);
}

void LocaleController::setLanguage(const QString &language)
{
    applyLanguage(language, true);
}

void LocaleController::setEngine(QQmlEngine *engine)
{
    m_engine = engine;
}

void LocaleController::applyLanguage(const QString &language, bool persist)
{
    const QString normalized = language.compare(QStringLiteral("zh_CN"), Qt::CaseInsensitive) == 0
            || language.compare(QStringLiteral("zh-CN"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("zh_CN")
        : QStringLiteral("en");
    if (normalized == m_language)
        return;

    if (m_translatorInstalled) {
        QCoreApplication::removeTranslator(&m_translator);
        m_translatorInstalled = false;
    }

    QString effectiveLanguage = normalized;
    if (normalized == QStringLiteral("zh_CN")) {
        if (m_translator.load(QStringLiteral(":/i18n/yanami_zh_CN.qm"))) {
            m_translatorInstalled = QCoreApplication::installTranslator(&m_translator);
        } else {
            effectiveLanguage = QStringLiteral("en");
        }
    }

    m_language = effectiveLanguage;
    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/language"), m_language);
        // The native launcher starts before Qt and reads this same preference.
        // Flush it now so an immediate close/relaunch cannot observe stale text.
        settings.sync();
    }
    emit languageChanged();
    if (m_engine)
        m_engine->retranslate();
}
