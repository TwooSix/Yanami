#pragma once

#include <QHash>
#include <QQueue>
#include <QString>
#include <QVector>

#include <algorithm>
#include <functional>
#include <utility>

// A small, event-loop-owned scheduling primitive for keyed mutations.
//
// Requests for the same key are dispatched strictly in enqueue order, while
// requests for different keys may occupy up to maxActiveKeys slots. Execution
// is deliberately left to the owner so this policy can be tested without
// threads, timers or a live backend.
template<typename Request>
class KeyedMutationScheduler final
{
public:
    using KeySelector = std::function<QString(const Request &)>;
    using Compatibility = std::function<bool(const Request &, const Request &)>;

    explicit KeyedMutationScheduler(
        qsizetype maxActiveKeys,
        KeySelector keySelector,
        Compatibility compatibility = {})
        : m_maxActiveKeys(std::max<qsizetype>(1, maxActiveKeys))
        , m_keySelector(std::move(keySelector))
        , m_compatibility(std::move(compatibility))
    {
    }

    bool enqueue(Request request)
    {
        const QString key = m_keySelector(request);
        if (key.isEmpty())
            return false;
        QQueue<Request> &queue = m_queues[key];
        const bool needsReadyEntry = queue.isEmpty() && !m_active.contains(key);
        queue.enqueue(std::move(request));
        if (needsReadyEntry)
            m_readyKeys.enqueue(key);
        return true;
    }

    QVector<Request> takeReady()
    {
        QVector<Request> ready;
        qsizetype candidatesRemaining = m_readyKeys.size();
        while (m_active.size() < m_maxActiveKeys
               && !m_readyKeys.isEmpty()
               && candidatesRemaining-- > 0) {
            const QString key = m_readyKeys.dequeue();
            if (m_active.contains(key))
                continue;
            auto found = m_queues.find(key);
            if (found == m_queues.end() || found->isEmpty())
                continue;
            const Request &candidate = found->head();
            const bool compatible = !m_compatibility
                || std::all_of(
                    m_active.cbegin(),
                    m_active.cend(),
                    [this, &candidate](const Request &active) {
                        return m_compatibility(candidate, active)
                            && m_compatibility(active, candidate);
                    });
            if (!compatible) {
                m_readyKeys.enqueue(key);
                continue;
            }
            Request request = found->dequeue();
            if (found->isEmpty())
                m_queues.erase(found);
            m_active.insert(key, request);
            ready.push_back(std::move(request));
        }
        return ready;
    }

    bool complete(const QString &key)
    {
        if (m_active.remove(key) == 0)
            return false;
        const auto found = m_queues.constFind(key);
        if (found != m_queues.cend() && !found->isEmpty())
            m_readyKeys.enqueue(key);
        return true;
    }

    QVector<Request> takeQueued()
    {
        QVector<Request> removed;
        for (auto iterator = m_queues.begin(); iterator != m_queues.end(); ++iterator) {
            while (!iterator->isEmpty())
                removed.push_back(iterator->dequeue());
        }
        m_queues.clear();
        m_readyKeys.clear();
        return removed;
    }

    template<typename Predicate>
    QVector<Request> takeQueuedIf(Predicate predicate)
    {
        QVector<Request> removed;
        QQueue<QString> readyKeys;
        while (!m_readyKeys.isEmpty()) {
            const QString key = m_readyKeys.dequeue();
            auto found = m_queues.find(key);
            if (found == m_queues.end())
                continue;
            QQueue<Request> retained;
            while (!found->isEmpty()) {
                Request request = found->dequeue();
                if (predicate(request))
                    removed.push_back(std::move(request));
                else
                    retained.enqueue(std::move(request));
            }
            if (retained.isEmpty()) {
                m_queues.erase(found);
            } else {
                m_queues.insert(key, std::move(retained));
                readyKeys.enqueue(key);
            }
        }

        // Queues behind active keys do not appear in m_readyKeys until their
        // active request completes, so filter those separately.
        for (auto iterator = m_queues.begin(); iterator != m_queues.end();) {
            if (!m_active.contains(iterator.key())) {
                ++iterator;
                continue;
            }
            QQueue<Request> retained;
            while (!iterator->isEmpty()) {
                Request request = iterator->dequeue();
                if (predicate(request))
                    removed.push_back(std::move(request));
                else
                    retained.enqueue(std::move(request));
            }
            if (retained.isEmpty()) {
                iterator = m_queues.erase(iterator);
            } else {
                *iterator = std::move(retained);
                ++iterator;
            }
        }
        m_readyKeys = std::move(readyKeys);
        return removed;
    }

    template<typename Predicate>
    bool anyOf(Predicate predicate) const
    {
        for (auto iterator = m_active.cbegin(); iterator != m_active.cend(); ++iterator) {
            if (predicate(iterator.value()))
                return true;
        }
        for (auto iterator = m_queues.cbegin(); iterator != m_queues.cend(); ++iterator) {
            for (const Request &request : iterator.value()) {
                if (predicate(request))
                    return true;
            }
        }
        return false;
    }

    QVector<Request> activeRequests() const
    {
        QVector<Request> requests;
        requests.reserve(m_active.size());
        for (auto iterator = m_active.cbegin(); iterator != m_active.cend(); ++iterator)
            requests.push_back(iterator.value());
        return requests;
    }

    qsizetype activeCount() const { return m_active.size(); }

    qsizetype queuedCount() const
    {
        qsizetype count = 0;
        for (auto iterator = m_queues.cbegin(); iterator != m_queues.cend(); ++iterator)
            count += iterator->size();
        return count;
    }

    bool active(const QString &key) const { return m_active.contains(key); }

private:
    qsizetype m_maxActiveKeys = 1;
    KeySelector m_keySelector;
    Compatibility m_compatibility;
    QHash<QString, QQueue<Request>> m_queues;
    QHash<QString, Request> m_active;
    QQueue<QString> m_readyKeys;
};
