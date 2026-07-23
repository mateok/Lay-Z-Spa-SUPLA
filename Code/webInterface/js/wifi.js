loadNetworkConfig();

function loadNetworkConfig() {
    var req = new XMLHttpRequest();
    req.open('POST', '/getwifi/');
    req.send();
    req.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
            var json = JSON.parse(req.responseText);
            console.log(json);
            document.getElementById('enableAp').checked        = !!json.enableAp;
            document.getElementById('apSsid').value            = json.apSsid    || '';
            document.getElementById('apPwd').value             = json.apPwd     || '';
            document.getElementById('enableWM').checked        = !!json.enableWM;

            document.getElementById('enableStaticIp4').checked = !!json.enableStaticIp4;
            document.getElementById('ip4Address').value         = json.ip4Address        || '';
            document.getElementById('ip4Gateway').value         = json.ip4Gateway        || '';
            document.getElementById('ip4Subnet').value          = json.ip4Subnet         || '';
            document.getElementById('ip4DnsPrimary').value      = json.ip4DnsPrimary     || '';
            document.getElementById('ip4DnsSecondary').value    = json.ip4DnsSecondary   || '';
            document.getElementById('ip4NTP').value             = json.ip4NTP            || '';

            // RF tuning. Defaults applied if missing from JSON (older firmware).
            document.getElementById('txPowerDbm').value = (json.txPowerDbm !== undefined) ? json.txPowerDbm : 20.5;
            document.getElementById('noModemSleep').checked = (json.noModemSleep !== undefined) ? !!json.noModemSleep : true;
            document.getElementById('phyMode').value = (json.phyMode !== undefined) ? json.phyMode : 0;
        }
    }
}

function saveNetworkConfig() {
    if (!validatePassword('apPwd')) return;

    var button = document.getElementById('save');
    var status = document.getElementById('wifiSaveStatus');
    button.disabled = true;
    status.textContent = document.getElementById('wifiTextSaving').textContent;
    status.style.color = '';

    var req = new XMLHttpRequest();
    req.open('POST', '/setwifi/');
    req.setRequestHeader('Content-Type', 'application/json');
    req.timeout = 8000;
    var json = {
    enableAp:         document.getElementById('enableAp').checked,
    apSsid:           document.getElementById('apSsid').value,
    apPwd:            document.getElementById('apPwd').value,
    enableWM:         document.getElementById('enableWM').checked,
    enableStaticIp4:  document.getElementById('enableStaticIp4').checked,
    ip4Address:       document.getElementById('ip4Address').value,
    ip4Gateway:       document.getElementById('ip4Gateway').value,
    ip4Subnet:        document.getElementById('ip4Subnet').value                        || '255.255.255.0',
    ip4DnsPrimary:    document.getElementById('ip4DnsPrimary').value,
    ip4DnsSecondary:  document.getElementById('ip4DnsSecondary').value,
    ip4NTP:           document.getElementById('ip4NTP').value,
    txPowerDbm:       parseFloat(document.getElementById('txPowerDbm').value),
    noModemSleep:     document.getElementById('noModemSleep').checked,
    phyMode:          parseInt(document.getElementById('phyMode').value, 10)
    };
    req.onreadystatechange = function() {
        if (this.readyState !== 4) return;
        if (this.status >= 200 && this.status < 300) {
            status.textContent = document.getElementById('wifiTextSaved').textContent;
            document.getElementById('apPwd').value = '<enter password>';
            return;
        }
        status.textContent = document.getElementById('wifiTextFailed').textContent;
        status.style.color = '#b00020';
        button.disabled = false;
    };
    req.onerror = req.ontimeout = function() {
        status.textContent = document.getElementById('wifiTextFailed').textContent;
        status.style.color = '#b00020';
        button.disabled = false;
    };
    req.send(JSON.stringify(json));
}
