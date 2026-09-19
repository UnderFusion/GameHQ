// Focused geometry regression checks without launching the production app.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const qml = name => fs.readFileSync(path.join(__dirname, '../src/ui/qml/components', name), 'utf8');
const source = qml('SettingsPage.qml');
const body = source.match(/function revealFocusedItem\(\) \{([\s\S]*?)\n    \}/)[1];
const pageContainer = {};
const flick = { contentY: 200, height: 300, contentHeight: 1000 };
const Window = { window: { activeFocusItem: null } };
const PadNav = { isInside: (item, parent) => item.parent === parent };
const reveal = new Function('Window', 'pageContainer', 'PadNav', 'padOverlay', 'Theme', 'flick', body);
const run = item => {
    Window.window.activeFocusItem = item;
    reveal(Window, pageContainer, PadNav, null, { s16: 16 }, flick);
};
const item = (y, height = 30) => ({ parent: pageContainer, height, mapToItem: () => ({ y }) });
run({ height: 300, mapToItem: () => ({ y: 0 }) });
assert.equal(flick.contentY, 200, 'viewport/background focus must not reposition content');
run(item(240));
assert.equal(flick.contentY, 200, 'visible controls stay put');
run(item(100));
assert.equal(flick.contentY, 84, 'navigation reveals controls above viewport');
run(item(600));
assert.equal(flick.contentY, 346, 'navigation reveals controls below viewport');
flick.contentY = 0;
flick.contentHeight = 290;
run(item(270, 20));
assert.equal(flick.contentY, 0, 'short pages must never acquire negative scroll offsets');

const grid = qml('DesktopGalleryGrid.qml');
const expression = grid.match(/galleryGrid.contentY = (galleryGrid.originY\s*\+ gridScroll.position \* galleryGrid.contentHeight)/)[1];
const position = new Function('galleryGrid', 'gridScroll', 'return ' + expression);
for (const originY of [0, -80]) {
    const view = { originY, height: 300, contentHeight: 1000 };
    assert.equal(position(view, { position: 0 }), originY);
    assert.equal(position(view, { position: 0.35 }), originY + 350);
    assert.equal(position(view, { position: 0.7 }), originY + 700, 'thumb reaches content bottom');
}
console.log('PASS: Settings focus geometry and gallery scrollbar mapping');
