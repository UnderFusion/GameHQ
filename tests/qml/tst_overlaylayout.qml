import QtQuick
import QtTest
import "../../src/ui/qml/components"

// Overlay options: thumbnail size drives the strip height (so the preview
// gains room), the options card exposes its rows by stable key, and the
// footer switches to the options hints while the card is open.
TestCase {
    name: "OverlayLayout"
    when: windowShown
    width: 1200
    height: 800

    Component {
        id: stripComponent
        OverlayCaptureStrip { width: 900 }
    }
    Component {
        id: panelComponent
        Item {
            width: 800; height: 700
            property alias panel: panel
            OverlayLayoutPanel { id: panel; open: true }
        }
    }
    Component {
        id: footerComponent
        Item {
            width: 1200; height: 100
            property alias footer: footer
            OverlayFooter { id: footer }
        }
    }

    function test_thumbnailScaleResizesStrip() {
        const strip = createTemporaryObject(stripComponent, this)
        verify(strip)
        compare(strip.height, 132 + 32)   // unchanged at 100%
        const full = strip.height
        strip.thumbScale = 0.7
        verify(strip.height < full)
        compare(strip.tileWidth, 126)
        strip.thumbScale = 1.4
        verify(strip.height > full)
        compare(strip.tileWidth, 252)
    }

    function test_panelRowsAndValues() {
        const host = createTemporaryObject(panelComponent, this)
        verify(host)
        const panel = host.panel
        compare(panel.rows.length, 8)
        compare(panel.rows.map(r => r.key),
                ["hints", "margin_left", "margin_top", "margin_right", "margin_bottom",
                 "scale", "thumbs", "reset"])
        panel.currentIndex = 5
        compare(panel.currentKey(), "scale")
        panel.currentIndex = 99
        compare(panel.currentKey(), "")
        panel.values = { hints: false, margin_left: 0, margin_top: 48, margin_right: 48,
                         margin_bottom: 48, scale: 75, thumbs: 100 }
        compare(panel.valueText(panel.rows[0]), panel.offLabel)
        compare(panel.valueText(panel.rows[1]), panel.pxFormat.arg(0))
        compare(panel.valueText(panel.rows[5]), "75%")
        compare(panel.valueText(panel.rows[7]), "")
        verify(panel.height <= host.height)
    }

    function test_footerShowsOptionsHints() {
        const host = createTemporaryObject(footerComponent, this)
        verify(host)
        const browse = host.footer.text
        host.footer.layoutPanelOpen = true
        verify(host.footer.text !== browse)
        host.footer.usingGamepad = true
        const pad = host.footer.text
        host.footer.usingGamepad = false
        verify(host.footer.text !== pad)
    }
}
