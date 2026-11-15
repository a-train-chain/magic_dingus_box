const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// Minimal DOM boundary: exercise the shipped render and change handlers.
class Element {
    constructor() { this.options=[]; this.dataset={}; this.events={}; this.style={}; this.value=''; }
    addEventListener(type, handler) { this.events[type]=handler; }
    appendChild(option) { this.options.push(option); }
    replaceChildren() { this.options=[]; }
    set innerHTML(value) { if (value==='') this.options=[]; }
    get selectedOptions() { return this.options.filter(o=>o.selected); }
}
test('primary change rebuilds eligible fallback choices and preserves other selections', async () => {
    const ids=['mbVpnAdvanced','mbVpnCountrySelect','mbVpnCountryApply','mbVpnCountryNote','mbVpnFallbackSelect','mbVpnCountryStatus'];
    const nodes=Object.fromEntries(ids.map(id=>[id,new Element()]));
    const context={document:{readyState:'complete',getElementById:id=>nodes[id],createElement:()=>new Element()},
        currentDevice:{url:'fixture'},apiGet:async()=>({country:'Canada',choices:['Canada','United States','Netherlands'],
            fallback_countries:['United States','Netherlands'],editable:true}),escapeHtml:s=>s,setTimeout:()=>{}};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../static/vpn_settings.js'),'utf8'),context);
    nodes.mbVpnAdvanced.open=true;nodes.mbVpnAdvanced.events.toggle();
    await new Promise(resolve=>setImmediate(resolve));
    const primary=nodes.mbVpnCountrySelect, fallback=nodes.mbVpnFallbackSelect;
    primary.value='United States';primary.events.change();
    assert.deepEqual(fallback.options.map(o=>o.value),['Canada','Netherlands']);
    assert.deepEqual(fallback.selectedOptions.map(o=>o.value),['Netherlands']);
    fallback.options[0].selected=true;fallback.events.change();
    assert.equal(nodes.mbVpnCountryApply.disabled,false);
    assert.deepEqual(fallback.selectedOptions.map(o=>o.value),['Canada','Netherlands']);
});
