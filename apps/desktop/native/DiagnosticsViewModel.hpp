#pragma once

#include "RuntimeLogger.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

class DiagnosticsViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool exporting READ exporting NOTIFY stateChanged)
    Q_PROPERTY(QString lastExportPath READ lastExportPath NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    explicit DiagnosticsViewModel(QObject *parent = nullptr);
    ~DiagnosticsViewModel() override;

    bool exporting() const { return m_exporting; }
    QString lastExportPath() const { return m_lastExportPath; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void exportLogs();
    Q_INVOKABLE void openExportFolder();

signals:
    void stateChanged();
    void exportSucceeded(const QString &path);
    void exportFailed(const QString &message);

private:
    QString nextExportPath() const;
    QString exportErrorMessage(RuntimeLogger::LogExportError error) const;
    void setFailure(const QString &message);
    void finishExport();

    QFutureWatcher<RuntimeLogger::LogExportResult> m_exportWatcher;
    QString m_lastExportPath;
    QString m_errorMessage;
    bool m_exporting = false;
};
