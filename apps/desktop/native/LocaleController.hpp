#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTranslator>

class QQmlEngine;

class LocaleController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)

public:
    explicit LocaleController(QQmlEngine *engine, QObject *parent = nullptr);

    QString language() const { return m_language; }
    void setEngine(QQmlEngine *engine);
    Q_INVOKABLE void setLanguage(const QString &language);

signals:
    void languageChanged();

private:
    void applyLanguage(const QString &language, bool persist);

    QPointer<QQmlEngine> m_engine;
    QTranslator m_translator;
    QString m_language;
    bool m_translatorInstalled = false;
};
