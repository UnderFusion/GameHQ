const assert = require('node:assert/strict');
const fs = require('node:fs');
const source = fs.readFileSync('src/ui/qml/components/AboutWhatsNewDialog.qml', 'utf8');
const functions = ['bundledUpdateRelease', 'selectedUsesRemoteNotes', 'selectedStructuredSections', 'summaryItems'];
const code = functions.map(name => source.match(new RegExp('    function ' + name + '\\(\\) \\{([\\s\\S]*?)\\n    \\}'))[0]).join('\n');
const run = new Function('app', 'bundledReleases', 'updateVersion', 'hasUpdateRelease', 'updateReleaseSelected', 'selectedBundledRelease', 'updates', code + '\nreturn {remote: selectedUsesRemoteNotes(), sections: selectedStructuredSections(), summary: summaryItems()};');
const installed = {version:'0.7.6', sections:[{items:['Installed notes']}]};
const localized = {version:'0.7.7', sections:[{items:['Polish update notes']}]};
let releases=[installed];
const check=(available,selected)=>run({releaseNotesSections:installed.sections},()=>releases,()=> '0.7.7',()=>available,selected,()=>installed,{noteBlocks:[{kind:'heading',text:'Changes'},{kind:'bullet',text:'Fetched update notes'}]});
let result=check(true,true);
assert.equal(result.remote,true,'future version uses fetched notes inside the dialog');
assert.deepEqual(result.sections,[],'never show installed notes under a future version');
assert.deepEqual(result.summary,['Fetched update notes']);
releases.push(localized);
result=check(true,true);
assert.equal(result.remote,false);
assert.deepEqual(result.sections,localized.sections,'matching verified localized notes are shown');
assert.deepEqual(result.summary,['Polish update notes']);
result=check(true,false);
assert.deepEqual(result.sections,installed.sections,'switching to installed history restores its notes');
assert.equal(result.remote,false);
assert.deepEqual(check(false,false).summary,['Installed notes']);
assert.match(source,/controls\.push\(retryReleaseNotesLink\)/,'controller navigation reaches retry');
assert.match(source,/id: retryReleaseNotesLink[\s\S]*?onClicked: updates\.retryNotes\(\)/);
assert.match(source,/model: root.selectedUsesRemoteNotes\(\) \? updates.noteBlocks : \[\]/);
console.log('PASS: fetched update notes, exact version matching, history switching and controller retry');

const formatCode = ['escapedStyledText', 'releaseBlockText'].map(name =>
    source.match(new RegExp('    function ' + name + '\\([^)]*\\) \\{([\\s\\S]*?)\\n    \\}'))[0]).join('\n');
const format = new Function(formatCode + '\nreturn releaseBlockText;')();
assert.equal(format({lead:'',text:'<img src="https://example.com"> & <script>x</script>'}),
    '&lt;img src=&quot;https://example.com&quot;&gt; &amp; &lt;script&gt;x&lt;/script&gt;');
assert.equal(format({lead:'<a href="x">Fix</a>',body:'<img src=x>'}),
    '<b>&lt;a href=&quot;x&quot;&gt;Fix&lt;/a&gt;</b> &lt;img src=x&gt;');
console.log('PASS: remote HTML and image markup remain escaped text; only UI-owned bold markup renders');
