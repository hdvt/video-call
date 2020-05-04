// hashmap
function HashMap() {
    var e = [];
    return e.size = function () {
        return this.length
    }, e.isEmpty = function () {
        return 0 === this.length
    }, e.containsKey = function (e) {
        e += "";
        for (var t = 0; t < this.length; t++)
            if (this[t].key === e) return t;
        return -1
    }, e.get = function (e) {
        e += "";
        var t = this.containsKey(e);
        if (t > -1) return this[t].value
    }, e.put = function (e, t) {
        if (e += "", -1 !== this.containsKey(e)) return this.get(e);
        this.push({
            key: e,
            value: t
        })
    }, e.allKeys = function () {
        for (var e = [], t = 0; t < this.length; t++) e.push(this[t].key);
        return e
    }, e.allIntKeys = function () {
        for (var e = [], t = 0; t < this.length; t++) e.push(parseInt(this[t].key));
        return e
    }, e.remove = function (e) {
        e += "";
        var t = this.containsKey(e);
        t > -1 && this.splice(t, 1)
    }, e.clear = function () {
        for (var e = this.allKeys(), t = 0; t < e.length; t++) {
            var r = e[t];
            this.remove(r)
        }
    }, e
}
var internalID;
var call_state = [
    "CALL_INIT",
    "CALL_BUSY",
    "CALL_RINGING",
    "CALL_ACCEPTED",
    "CALL_REJECT",
    "CALL_MISSED",
    "CALL_STARTED",
    "CALL_TIMEOUT",
    "CALL_ENDED"
];
// VideoCall class
function VideoCall() {
    this.isInited = false;
    this.media_server = null;
    this.admin_server = null;
    this.janus = null;
    this.plugin = null;
    this.plugin_name = null;
    this._onMethods = null;
    this.myname = null;
    this.peername = null;
    this.isConnected = false;
    this.isAttached = false;
    this.videoenabled = true;
    this.audioenabled = true;
    this.simulcastStarted = false;
    this.jsep = {
        offer: null,
        answer: null
    };
}

// init 
VideoCall.prototype.init = function (callback) {
    if (!Janus.isWebrtcSupported()) {
        callback.error("No WebRTC support... ");
        return;
    }
    Janus.init({
        debug: true,
        callback: (function () {
            this.media_server = "http://" + window.location.hostname + ":8088/janus";
            this.admin_server = "http://" + window.location.hostname + ":7088/admin";
            this.plugin_name = "janus.plugin.videocall";
            this._onMethods = new HashMap();
            this.isInited = true;
            callback.success();
        }).bind(this)
    });
}

// add event to _onMethods 
VideoCall.prototype.on = function (e, t) {
    this._onMethods.put(e, t);
}

// call event in _onMethods
VideoCall.prototype.callOnEvent = function (e, t) {
    var r = this._onMethods.get(e);
    r ? t ? r.call(this, t) : r.call(this) : console.log("Please implement event: " + e)
}

// check init
VideoCall.prototype.isInit = function () {
    return this.isInited;
}
// connect to server
VideoCall.prototype.connect = function (account, callback) {
    var self = this;
    self.janus = new Janus(
        {
            server: this.media_server,
            iceServers: [
                {
                    'urls': 'stun:bangtv.ml:3478'
                },
                {
                    'urls': 'turn:bangtv.ml:3478?transport=tcp',
                    'credential': '1231234',
                    'username': 'bangtran'
                },
                {
                    'urls': 'turn:bangtv.ml:3478?transport=udp',
                    'credential': '1231234',
                    'username': 'bangtran'
                },
                {
                    'urls': 'turn:bangtv.ml:443?transport=tcp',
                    'credential': '1231234',
                    'username': 'bangtran'
                }
            ],
            token: account,
            success: function () {
                self.isConnected = true;
                self.janus.attach(
                    {
                        plugin: self.plugin_name,
                        opaqueId: "videocalltest-" + Janus.randomString(12),
                        success: function (pluginHandle) {
                            self.plugin = pluginHandle;
                            self.isAttached = true;
                            //self.callOnEvent('connected');
                            var register = { "request": "login", "username": account };
                            self.plugin.send({ "message": register });
                            console.debug("Plugin attached! (" + self.plugin.getPlugin() + ", id=" + self.plugin.getId() + ")");
                        },
                        onlocalstream: function (stream) {
                            console.debug("onlocalstream");
                            self.callOnEvent('addlocalstream', stream);
                        },
                        onremotestream: function (stream) {
                            console.debug("onremotestream");
                            self.callOnEvent('addremotestream', stream);
                        },
                        onmessage: function (msg, jsep) {
                            Janus.debug(" ::: Got a message :::");
                            Janus.debug(msg);
                            var result = msg["result"];
                            if (result !== null && result !== undefined) {
                                if (result["event"] !== undefined && result["event"] !== null) {
                                    var event = result["event"];
                                    if (event === 'connected') {
                                        self.myname = result["username"];
                                        console.debug("Successfully connected as username: " + self.myname + "!")
                                        callback.success();
                                    } else if (event === 'calling') {
                                        console.debug("Waiting for the peer to answer...");
                                        self.callOnEvent('calling');
                                    } else if (event === 'incomingcall') {
                                        console.debug("Incoming call from " + result["username"] + "!");
                                        self.peername = result["username"];
                                        self.jsep.answer = jsep;
                                        self.ringing(true);
                                        self.callOnEvent('incomingcall', self.peername);
                                    } else if (event === 'accepted') {
                                        var peer = result["username"];
                                        if (peer === null || peer === undefined) {
                                            console.debug("Call started!");
                                            self.ringing(false);
                                        } else {
                                            console.debug(peer + " accepted the call!");
                                            self.peername = peer;
                                        }
                                        if (jsep)
                                            self.plugin.handleRemoteJsep({ jsep: jsep });
                                        self.callOnEvent('answered');
                                    } else if (event === 'update') {
                                        if (jsep) {
                                            if (jsep.type === "answer") {
                                                self.plugin, handleRemoteJsep({ jsep: jsep });
                                            } else {
                                                self.plugin, createAnswer(
                                                    {
                                                        jsep: jsep,
                                                        media: { data: true },
                                                        simulcast: false,
                                                        success: function (jsep) {
                                                            Janus.debug("Got SDP!");
                                                            Janus.debug(jsep);
                                                            var body = { "request": "set" };
                                                            self.plugin, send({ "message": body, "jsep": jsep });
                                                        },
                                                        error: function (error) {
                                                            Janus.error("WebRTC error:", error);
                                                        }
                                                    });
                                            }
                                        }
                                    } else if (event === 'stop') {
                                        console.debug("Stop event: " + call_state[result["call_state"]]);
                                        switch (call_state[result["call_state"]]) {
                                            case "CALL_ENDED":
                                            case "CALL_TIMEOUT":
                                                console.debug("+ Start time: " + result["start_time"]);
                                                console.debug("+ Stop time: " + result["stop_time"]);
                                                if (result["record_path"])
                                                    console.debug("+ Record path: " + result["record_path"]);
                                                break;
                                            case "CALL_ACCEPTED":
                                                self.ringing(false);
                                                break;
                                            case "CALL_BUSY":
                                                bootbox.alert("Callee is busy now");
                                                break;
                                        }
                                        self.plugin.hangup();
                                        self.callOnEvent('stop', result["call_state"]);
                                    }
                                    else if (event === "timeout") {
                                        self.hangup();
                                        self.ringing(false);
                                        console.debug("The call timeout. Hangup by user " + result["username"]);
                                    }
                                    else if (event === "simulcast") {
                                        // Is simulcast in place?
                                        console.debug("Simulcast event");
                                        var substream = result["substream"];
                                        var temporal = result["temporal"];
                                        if ((substream !== null && substream !== undefined) || (temporal !== null && temporal !== undefined)) {
                                            if (!self.simulcastStarted) {
                                                self.simulcastStarted = true;
                                                self.addSimulcastButtons(result["videocodec"] === "vp8" || result["videocodec"] === "h264");
                                            }
                                            // We just received notice that there's been a switch, update the buttons
                                            self.updateSimulcastButtons(substream, temporal);
                                        }
                                    }
                                }
                            } else {
                                // FIXME Error?
                                var error = msg["error"];
                                bootbox.alert("Error: " + error);
                                if (error.indexOf("already taken") > 0) {
                                    // FIXME Use status codes...
                                    callback.error("Username has already taken");
                                }
                                // TODO Reset status
                                self.plugin.hangup();
                                self.callOnEvent('stop', "error");
                            }
                        },
                        error: function (error) {
                            Janus.error("  -- Error attaching plugin...", error);
                        }
                    });
            },
            error: function (error) {
                callback.error(error);
            },
            destroyed: function () {
                window.location.reload();
            }
        });
}

VideoCall.prototype.disconnect = function () {
    this.plugin.detach();
}
// register user
VideoCall.prototype.register = function (token, callback) {
    var self = this;
    var request = { "janus": "add_token", "token": token, plugins: ["janus.plugin.videocall"], "transaction": Janus.randomString(12), "admin_secret": "1231234" };
    $.ajax({
        type: 'POST',
        url: self.admin_server,
        cache: false,
        contentType: "application/json",
        data: JSON.stringify(request),
        success: function (json) {
            if (json["janus"] !== "success") {
                callback.error(json["error"].code + " - " + json["error"].reason);
                return;
            } else {
                callback.success();
            }
        },
        error: function (XMLHttpRequest, textStatus, errorThrown) {
            callback.error(textStatus + ": " + errorThrown);
        },
        dataType: "json"
    });
}

// make a call
VideoCall.prototype.makeCall = function (peer, options) {
    // Call this user
    var self = this;
    self.plugin.hangup();
    if (options.stream) {
        console.log("Local stream: " + options.stream);
    }
    this.plugin.createOffer(
        {
            media: { data: false },
            stream: options.stream ? options.stream : null,
            simulcast: false,
            success: function (jsep) {
                Janus.debug("Got SDP!");
                Janus.debug(jsep);
                self.jsep.offer = jsep;
                var body = {
                    "request": "call",
                    "username": peer,
                    'videocall': options.isVideoCall ? options.isVideoCall : true,
                    'record': options.isRecording ? options.isRecording : false,
                    'duration': options.duration ? options.duration : null
                };
                self.plugin.send({ "message": body, "jsep": jsep });
                Janus.debug("Call message: " + body);
            },
            error: function (error) {
                Janus.error("WebRTC error...", error);
            }
        });
}

// answer a call
VideoCall.prototype.answer = function (options) {
    var self = this;
    this.plugin.createAnswer(
        {
            jsep: self.jsep.answer,
            media: { data: false },
            simulcast: false,
            stream: options.stream ? options.stream : null,
            success: function (jsep) {
                Janus.debug("Got SDP!");
                Janus.debug(jsep);
                self.jsep.offer = jsep;
                var body = { "request": "accept" };
                self.plugin.send({ "message": body, "jsep": jsep });
                options.success();
            },
            error: function (error) {
                options.error(error);
            }
        });
}

// ringing
VideoCall.prototype.ringing = function (status) {
    var self = this;
    if (status) {
        internalID = setInterval(function () {
            console.debug("setInterval");
            self.plugin.send({ "message": { "request": "ringing" } });
        }, 1000);
    }
    else {
        if (internalID != null) {
            clearInterval(internalID);
            internalID = null;
        }
    }
}
// mute a call
VideoCall.prototype.mute = function (isMuted) {
    this.audioenabled = isMuted;
    this.plugin.send({ "message": { "request": "set", "audio": this.audioenabled } });
}

// disable video
VideoCall.prototype.enableVideo = function (isEnable) {
    this.videoenabled = isEnable;
    this.plugin.send({ "message": { "request": "set", "video": this.videoenabled } });
}

// reject a call
VideoCall.prototype.reject = function () {
    var hangup = { "request": "reject" };
    this.plugin.send({ "message": hangup });
}

// hangup a call
VideoCall.prototype.hangup = function () {
    var hangup = { "request": "hangup" };
    this.plugin.send({ "message": hangup });
    //this.plugin.hangup();
}



// temp functions
// Helpers to create Simulcast-related UI, if enabled
VideoCall.prototype.addSimulcastButtons = function (temporal) {
    var self = this;
    $('#curres').parent().append(
        '<div id="simulcast" class="btn-group-vertical btn-group-vertical-xs pull-right">' +
        '	<div class"row">' +
        '		<div class="btn-group btn-group-xs" style="width: 100%">' +
        '			<button id="sl-2" type="button" class="btn btn-primary" data-toggle="tooltip" title="Switch to higher quality" style="width: 33%">SL 2</button>' +
        '			<button id="sl-1" type="button" class="btn btn-primary" data-toggle="tooltip" title="Switch to normal quality" style="width: 33%">SL 1</button>' +
        '			<button id="sl-0" type="button" class="btn btn-primary" data-toggle="tooltip" title="Switch to lower quality" style="width: 34%">SL 0</button>' +
        '		</div>' +
        '	</div>' +
        '	<div class"row">' +
        '		<div class="btn-group btn-group-xs hide" style="width: 100%">' +
        '			<button id="tl-2" type="button" class="btn btn-primary" data-toggle="tooltip" title="Cap to temporal layer 2" style="width: 34%">TL 2</button>' +
        '			<button id="tl-1" type="button" class="btn btn-primary" data-toggle="tooltip" title="Cap to temporal layer 1" style="width: 33%">TL 1</button>' +
        '			<button id="tl-0" type="button" class="btn btn-primary" data-toggle="tooltip" title="Cap to temporal layer 0" style="width: 33%">TL 0</button>' +
        '		</div>' +
        '	</div>' +
        '</div>');
    // Enable the simulcast selection buttons
    $('#sl-0').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Switching simulcast substream, wait for it... (lower quality)", null, { timeOut: 2000 });
            if (!$('#sl-2').hasClass('btn-success'))
                $('#sl-2').removeClass('btn-primary btn-info').addClass('btn-primary');
            if (!$('#sl-1').hasClass('btn-success'))
                $('#sl-1').removeClass('btn-primary btn-info').addClass('btn-primary');
            $('#sl-0').removeClass('btn-primary btn-info btn-success').addClass('btn-info');
            self.plugin.send({ message: { request: "set", substream: 0 } });
        });
    $('#sl-1').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Switching simulcast substream, wait for it... (normal quality)", null, { timeOut: 2000 });
            if (!$('#sl-2').hasClass('btn-success'))
                $('#sl-2').removeClass('btn-primary btn-info').addClass('btn-primary');
            $('#sl-1').removeClass('btn-primary btn-info btn-success').addClass('btn-info');
            if (!$('#sl-0').hasClass('btn-success'))
                $('#sl-0').removeClass('btn-primary btn-info').addClass('btn-primary');
            self.plugin.send({ message: { request: "set", substream: 1 } });
        });
    $('#sl-2').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Switching simulcast substream, wait for it... (higher quality)", null, { timeOut: 2000 });
            $('#sl-2').removeClass('btn-primary btn-info btn-success').addClass('btn-info');
            if (!$('#sl-1').hasClass('btn-success'))
                $('#sl-1').removeClass('btn-primary btn-info').addClass('btn-primary');
            if (!$('#sl-0').hasClass('btn-success'))
                $('#sl-0').removeClass('btn-primary btn-info').addClass('btn-primary');
            self.plugin.send({ message: { request: "set", substream: 2 } });
        });
    if (!temporal)	// No temporal layer support
        return;
    $('#tl-0').parent().removeClass('hide');
    $('#tl-0').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Capping simulcast temporal layer, wait for it... (lowest FPS)", null, { timeOut: 2000 });
            if (!$('#tl-2').hasClass('btn-success'))
                $('#tl-2').removeClass('btn-primary btn-info').addClass('btn-primary');
            if (!$('#tl-1').hasClass('btn-success'))
                $('#tl-1').removeClass('btn-primary btn-info').addClass('btn-primary');
            $('#tl-0').removeClass('btn-primary btn-info btn-success').addClass('btn-info');
            self.plugin.send({ message: { request: "set", temporal: 0 } });
        });
    $('#tl-1').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Capping simulcast temporal layer, wait for it... (medium FPS)", null, { timeOut: 2000 });
            if (!$('#tl-2').hasClass('btn-success'))
                $('#tl-2').removeClass('btn-primary btn-info').addClass('btn-primary');
            $('#tl-1').removeClass('btn-primary btn-info').addClass('btn-info');
            if (!$('#tl-0').hasClass('btn-success'))
                $('#tl-0').removeClass('btn-primary btn-info').addClass('btn-primary');
            self.plugin.send({ message: { request: "set", temporal: 1 } });
        });
    $('#tl-2').removeClass('btn-primary btn-success').addClass('btn-primary')
        .unbind('click').click(function () {
            toastr.info("Capping simulcast temporal layer, wait for it... (highest FPS)", null, { timeOut: 2000 });
            $('#tl-2').removeClass('btn-primary btn-info btn-success').addClass('btn-info');
            if (!$('#tl-1').hasClass('btn-success'))
                $('#tl-1').removeClass('btn-primary btn-info').addClass('btn-primary');
            if (!$('#tl-0').hasClass('btn-success'))
                $('#tl-0').removeClass('btn-primary btn-info').addClass('btn-primary');
            self.plugin.send({ message: { request: "set", temporal: 2 } });
        });
}

VideoCall.prototype.updateSimulcastButtons = function (substream, temporal) {
    // Check the substream
    if (substream === 0) {
        toastr.success("Switched simulcast substream! (lower quality)", null, { timeOut: 2000 });
        $('#sl-2').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#sl-1').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#sl-0').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
    } else if (substream === 1) {
        toastr.success("Switched simulcast substream! (normal quality)", null, { timeOut: 2000 });
        $('#sl-2').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#sl-1').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
        $('#sl-0').removeClass('btn-primary btn-success').addClass('btn-primary');
    } else if (substream === 2) {
        toastr.success("Switched simulcast substream! (higher quality)", null, { timeOut: 2000 });
        $('#sl-2').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
        $('#sl-1').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#sl-0').removeClass('btn-primary btn-success').addClass('btn-primary');
    }
    // Check the temporal layer
    if (temporal === 0) {
        toastr.success("Capped simulcast temporal layer! (lowest FPS)", null, { timeOut: 2000 });
        $('#tl-2').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#tl-1').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#tl-0').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
    } else if (temporal === 1) {
        toastr.success("Capped simulcast temporal layer! (medium FPS)", null, { timeOut: 2000 });
        $('#tl-2').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#tl-1').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
        $('#tl-0').removeClass('btn-primary btn-success').addClass('btn-primary');
    } else if (temporal === 2) {
        toastr.success("Capped simulcast temporal layer! (highest FPS)", null, { timeOut: 2000 });
        $('#tl-2').removeClass('btn-primary btn-info btn-success').addClass('btn-success');
        $('#tl-1').removeClass('btn-primary btn-success').addClass('btn-primary');
        $('#tl-0').removeClass('btn-primary btn-success').addClass('btn-primary');
    }
}