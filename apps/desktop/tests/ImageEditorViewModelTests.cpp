#include "AsyncResourceState.hpp"
#include "ImageEditorViewModel.hpp"

#include <QSignalSpy>
#include <QTest>

namespace {

QVariantMap image(
    const QString &type,
    const QVariant &index,
    const QString &previewUrl)
{
    return QVariantMap{
        {QStringLiteral("imageType"), type},
        {QStringLiteral("imageIndex"), index},
        {QStringLiteral("width"), 100},
        {QStringLiteral("height"), 200},
        {QStringLiteral("previewUrl"), previewUrl},
    };
}

QVariantMap editor(const QString &id, const QVariantList &images)
{
    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), QStringLiteral("Title ") + id},
        {QStringLiteral("images"), images},
        {QStringLiteral("providers"), QVariantList{}},
    };
}

QVariantMap slotFor(const QVariantList &slotModels, const QString &type)
{
    for (const QVariant &value : slotModels) {
        const QVariantMap slot = value.toMap();
        if (slot.value(QStringLiteral("imageType")).toString() == type)
            return slot;
    }
    return {};
}

QVariantMap imageAt(const QVariantList &images, const QString &type, int index)
{
    for (const QVariant &value : images) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("imageType")).toString() == type
            && candidate.value(QStringLiteral("imageIndex")).toInt() == index) {
            return candidate;
        }
    }
    return {};
}

} // namespace

class ImageEditorViewModelTests final : public QObject
{
    Q_OBJECT

private slots:
    void qmlSearchIntentDoesNotCollideWithResourceState()
    {
        ImageEditorViewModel model;
        const QMetaObject *metaObject = model.metaObject();

        QVERIFY(metaObject->indexOfMethod("search(QVariantMap)") >= 0);
        QVERIFY(metaObject->indexOfProperty("search") < 0);
        QVERIFY(metaObject->indexOfProperty("searchState") >= 0);
    }

    void qmlSurfaceHasConcreteFailuresAndNoGenericMutationDispatcher()
    {
        ImageEditorViewModel model;
        const QMetaObject *metaObject = model.metaObject();

        QVERIFY(metaObject->indexOfMethod(
            "beginMutation(QString,QVariantMap,QVariantMap)") < 0);
        QVERIFY(metaObject->indexOfSignal(
            "requestFailed(QString,QString,QString,bool)") < 0);
        QVERIFY(metaObject->indexOfSignal(
            "editorRequestFailed(QString,QString,bool)") >= 0);
        QVERIFY(metaObject->indexOfSignal(
            "providersRequestFailed(QString,QString,bool)") >= 0);
        QVERIFY(metaObject->indexOfSignal(
            "searchRequestFailed(QString,QString,bool)") >= 0);
        QVERIFY(metaObject->indexOfSignal(
            "mutationRequestFailed(QString,QString,bool)") >= 0);
        QVERIFY(metaObject->indexOfSignal(
            "reconciliationRequestFailed(QString,QString,bool)") >= 0);
    }

    void buildsFixedEmptySingleAndDuplicateSlots()
    {
        ImageEditorViewModel model;
        model.setSessionGeneration(7);
        const QVariantMap request = model.open({
            {QStringLiteral("id"), QStringLiteral("item-1")},
            {QStringLiteral("title"), QStringLiteral("Item")},
        });
        QVERIFY(model.applyEditor(editor(QStringLiteral("item-1"), {
            image(QStringLiteral("Primary"), 2, QStringLiteral("primary-2")),
            image(QStringLiteral("Logo"), {}, QStringLiteral("logo")),
            image(QStringLiteral("Primary"), 0, QStringLiteral("primary-0")),
        }), request));

        QCOMPARE(model.slotsModel().size(), 6);
        const QStringList expectedTypes{
            QStringLiteral("Primary"),
            QStringLiteral("Logo"),
            QStringLiteral("Thumb"),
            QStringLiteral("Banner"),
            QStringLiteral("Disc"),
            QStringLiteral("Art"),
        };
        for (int index = 0; index < expectedTypes.size(); ++index) {
            QCOMPARE(model.slotsModel().at(index).toMap()
                .value(QStringLiteral("imageType")).toString(), expectedTypes.at(index));
        }

        const QVariantMap primary = slotFor(model.slotsModel(), QStringLiteral("Primary"));
        QCOMPARE(primary.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(primary.value(QStringLiteral("extraCount")).toInt(), 1);
        QVERIFY(primary.value(QStringLiteral("hasMultiple")).toBool());
        QCOMPARE(primary.value(QStringLiteral("image")).toMap()
            .value(QStringLiteral("imageIndex")).toInt(), 0);
        const QVariantList allPrimary = primary.value(QStringLiteral("images")).toList();
        QCOMPARE(allPrimary.at(0).toMap().value(QStringLiteral("imageIndex")).toInt(), 0);
        QCOMPARE(allPrimary.at(1).toMap().value(QStringLiteral("imageIndex")).toInt(), 2);

        const QVariantMap logo = slotFor(model.slotsModel(), QStringLiteral("Logo"));
        QCOMPARE(logo.value(QStringLiteral("count")).toInt(), 1);
        QVERIFY(logo.value(QStringLiteral("hasImage")).toBool());
        QVERIFY(!logo.value(QStringLiteral("imageIndex")).isValid());

        const QVariantMap art = slotFor(model.slotsModel(), QStringLiteral("Art"));
        QCOMPARE(art.value(QStringLiteral("count")).toInt(), 0);
        QVERIFY(!art.value(QStringLiteral("hasImage")).toBool());
        QVERIFY(art.value(QStringLiteral("image")).toMap().isEmpty());
    }

    void sortsBackdropsAndFindsFirstFreeIndex()
    {
        ImageEditorViewModel model;
        const QVariantMap request = model.open({
            {QStringLiteral("id"), QStringLiteral("item-1")},
        });
        QVERIFY(model.applyEditor(editor(QStringLiteral("item-1"), {
            image(QStringLiteral("Backdrop"), 3, QStringLiteral("three")),
            image(QStringLiteral("Backdrop"), 0, QStringLiteral("zero")),
            image(QStringLiteral("Backdrop"), 1, QStringLiteral("one")),
        }), request));

        QCOMPARE(model.backdropsModel().size(), 3);
        QCOMPARE(model.backdropsModel().at(0).toMap()
            .value(QStringLiteral("imageIndex")).toInt(), 0);
        QCOMPARE(model.backdropsModel().at(1).toMap()
            .value(QStringLiteral("imageIndex")).toInt(), 1);
        QCOMPARE(model.backdropsModel().at(2).toMap()
            .value(QStringLiteral("imageIndex")).toInt(), 3);
        QCOMPARE(model.nextBackdropIndex(), 2);
    }

    void rejectsLateItemAndDismissedResponses()
    {
        ImageEditorViewModel model;
        model.setSessionGeneration(11);
        const QVariantMap itemA = model.open({
            {QStringLiteral("id"), QStringLiteral("A")},
        });
        const QVariantMap itemB = model.open({
            {QStringLiteral("id"), QStringLiteral("B")},
        });

        QVERIFY(!model.applyEditor(editor(QStringLiteral("A"), {}), itemA));
        QCOMPARE(model.itemId(), QStringLiteral("B"));
        QVERIFY(model.applyEditor(editor(QStringLiteral("B"), {}), itemB));
        QCOMPARE(model.editor().value(QStringLiteral("id")).toString(), QStringLiteral("B"));

        const QVariantMap dismissed = model.open({
            {QStringLiteral("id"), QStringLiteral("dismissed")},
        });
        model.dismiss();
        QVERIFY(!model.opened());
        QVERIFY(!model.applyEditor(editor(QStringLiteral("dismissed"), {}), dismissed));
        QVERIFY(!model.opened());
    }

    void validatesEveryInitialRequestIdentityDimension()
    {
        const QStringList dimensions{
            QStringLiteral("requestId"),
            QStringLiteral("resourceKey"),
            QStringLiteral("sessionGeneration"),
            QStringLiteral("viewGeneration"),
        };
        for (const QString &dimension : dimensions) {
            ImageEditorViewModel model;
            model.setSessionGeneration(4);
            QVariantMap request = model.open({
                {QStringLiteral("id"), QStringLiteral("item")},
            });
            if (dimension == QStringLiteral("resourceKey"))
                request[dimension] = QStringLiteral("images:somewhere-else");
            else
                request[dimension] = request.value(dimension).toULongLong() + 1;
            QVERIFY2(!model.applyEditor(editor(QStringLiteral("item"), {}), request),
                qPrintable(dimension));
        }
    }

    void latestSearchGenerationWins()
    {
        ImageEditorViewModel model;
        const QVariantMap initial = model.open({
            {QStringLiteral("id"), QStringLiteral("item")},
        });
        QVERIFY(model.applyEditor(editor(QStringLiteral("item"), {}), initial));
        model.selectTarget(QStringLiteral("Primary"), 2, QStringLiteral("replace"));
        const QVariantMap first = model.beginSearch({
            {QStringLiteral("providerName"), QStringLiteral("first")},
            {QStringLiteral("includeAllLanguages"), false},
        });
        const QVariantMap second = model.beginSearch({
            {QStringLiteral("providerName"), QStringLiteral("second")},
            {QStringLiteral("includeAllLanguages"), true},
        });
        QVERIFY(first.value(QStringLiteral("searchGeneration")).toULongLong()
            < second.value(QStringLiteral("searchGeneration")).toULongLong());
        QVERIFY(!model.applySearch({
            {QStringLiteral("imageType"), QStringLiteral("Primary")},
            {QStringLiteral("images"), QVariantList{}},
        }, first));
        QVERIFY(model.applySearch({
            {QStringLiteral("imageType"), QStringLiteral("Primary")},
            {QStringLiteral("images"), QVariantList{}},
        }, second));
        QCOMPARE(model.searchState()->phase(), AsyncResourceState::Phase::Ready);
    }

    void mutationCarriesExactIndexAndFailureRollsBackOnlyThatCard()
    {
        ImageEditorViewModel model;
        model.setSessionGeneration(3);
        const QVariantMap initial = model.open({
            {QStringLiteral("id"), QStringLiteral("item")},
        });
        QVERIFY(model.applyEditor(editor(QStringLiteral("item"), {
            image(QStringLiteral("Primary"), 0, QStringLiteral("zero-old")),
            image(QStringLiteral("Primary"), 2, QStringLiteral("two-old")),
            image(QStringLiteral("Logo"), 0, QStringLiteral("logo-old")),
        }), initial));

        const QVariantMap target = model.selectTarget(
            QStringLiteral("Primary"),
            2,
            QStringLiteral("replace"));
        const QVariantMap mutation = model.applyRemote(
            target,
            image(QStringLiteral("Primary"), 99, QStringLiteral("two-new")));
        QVERIFY(!mutation.isEmpty());
        QCOMPARE(mutation.value(QStringLiteral("imageIndex")).toInt(), 2);
        QCOMPARE(mutation.value(QStringLiteral("imageType")).toString(),
            QStringLiteral("Primary"));
        QCOMPARE(imageAt(model.editor().value(QStringLiteral("images")).toList(),
            QStringLiteral("Primary"), 2).value(QStringLiteral("previewUrl")).toString(),
            QStringLiteral("two-new"));
        QCOMPARE(model.mutationStates()
            .value(model.cardKey(target)).toMap()
            .value(QStringLiteral("phase")).toString(), QStringLiteral("submitting"));

        QVERIFY(model.mutationFailed(mutation, QStringLiteral("server rejected it")));
        const QVariantList restored = model.editor().value(QStringLiteral("images")).toList();
        QCOMPARE(imageAt(restored, QStringLiteral("Primary"), 0)
            .value(QStringLiteral("previewUrl")).toString(), QStringLiteral("zero-old"));
        QCOMPARE(imageAt(restored, QStringLiteral("Primary"), 2)
            .value(QStringLiteral("previewUrl")).toString(), QStringLiteral("two-old"));
        QCOMPARE(imageAt(restored, QStringLiteral("Logo"), 0)
            .value(QStringLiteral("previewUrl")).toString(), QStringLiteral("logo-old"));
        const QVariantMap failedState = model.mutationStates()
            .value(model.cardKey(target)).toMap();
        QCOMPARE(failedState.value(QStringLiteral("phase")).toString(),
            QStringLiteral("error"));
        QCOMPARE(failedState.value(QStringLiteral("errorMessage")).toString(),
            QStringLiteral("server rejected it"));
    }

    void typedPublicIntentsKeepExplicitTargetContext()
    {
        ImageEditorViewModel model;
        const QVariantMap initial = model.open({
            {QStringLiteral("id"), QStringLiteral("item")},
        });
        QVERIFY(model.applyEditor(editor(QStringLiteral("item"), {
            image(QStringLiteral("Primary"), 2, QStringLiteral("old")),
        }), initial));

        const QVariantMap target = model.selectTarget(
            QStringLiteral("Primary"), 2, QStringLiteral("replace"));
        const QVariantMap search = model.search({
            {QStringLiteral("imageType"), QStringLiteral("Primary")},
            {QStringLiteral("providerName"), QStringLiteral("provider")},
        });
        QCOMPARE(search.value(QStringLiteral("target")).toMap(), target);
        model.cancelSearch();

        const QVariantMap apply = model.applyRemote(target,
            image(QStringLiteral("Primary"), 2, QStringLiteral("remote")));
        QCOMPARE(apply.value(QStringLiteral("kind")).toString(),
            QStringLiteral("apply"));
        QCOMPARE(apply.value(QStringLiteral("imageIndex")).toInt(), 2);
        QVERIFY(model.mutationFailed(apply, QStringLiteral("retry")));

        const QVariantMap upload = model.upload(
            target, QStringLiteral("file:///replacement.png"));
        QCOMPARE(upload.value(QStringLiteral("kind")).toString(),
            QStringLiteral("upload"));
        QVERIFY(model.mutationFailed(upload, QStringLiteral("retry")));

        const QVariantMap remove = model.remove(target);
        QCOMPARE(remove.value(QStringLiteral("kind")).toString(),
            QStringLiteral("delete"));
        QVERIFY(model.mutationFailed(remove, QStringLiteral("retry")));

        QSignalSpy retrySpy(model.initial(), &AsyncResourceState::retryRequested);
        model.retry();
        QCOMPARE(retrySpy.count(), 1);
    }
};

QTEST_MAIN(ImageEditorViewModelTests)
#include "ImageEditorViewModelTests.moc"
