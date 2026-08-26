import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    name: "LocaleText"

    function test_typeTokensUseLocalizedLabels() {
        compare(LocaleText.mediaSubtitle({
            "itemType": "Movie",
            "subtitle": ""
        }), LocaleText.itemTypeLabel("Movie"))
        compare(LocaleText.mediaSubtitle({
            "itemType": "Movie",
            "subtitle": "Movie"
        }), LocaleText.itemTypeLabel("Movie"))
        compare(LocaleText.mediaSubtitle({
            "itemType": "MusicVideo",
            "subtitle": "MusicVideo"
        }), LocaleText.itemTypeLabel("MusicVideo"))
        compare(LocaleText.mediaSubtitle({
            "itemType": "Season",
            "subtitle": "Series",
            "childCount": 0,
            "productionYear": 0
        }), LocaleText.itemTypeLabel("Season"))
    }

    function test_semanticSubtitlesRemainUntouched() {
        compare(LocaleText.mediaSubtitle({
            "itemType": "Movie",
            "subtitle": "2026"
        }), "2026")
        compare(LocaleText.mediaSubtitle({
            "itemType": "Episode",
            "subtitle": "S01E03 · Pilot"
        }), "S01E03 · Pilot")
        compare(LocaleText.mediaSubtitle({
            "itemType": "FutureType",
            "subtitle": "Server-provided label"
        }), "Server-provided label")
    }

    function test_unknownEmptySubtitleUsesGenericFallback() {
        compare(LocaleText.mediaSubtitle({
            "itemType": "FutureType",
            "subtitle": ""
        }), LocaleText.itemTypeLabel("FutureType"))
    }
}
