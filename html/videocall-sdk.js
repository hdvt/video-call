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
                            Janus.log("Plugin attached! (" + self.plugin.getPlugin() + ", id=" + self.plugin.getId() + ")");
                        },
                        onlocalstream: function (stream) {
                            Janus.log("onlocalstream");
                            self.callOnEvent('addlocalstream', stream);
                        },
                        onremotestream: function (stream) {
                            Janus.log("onremotestream");
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
                                        Janus.log("Successfully connected as username: " + self.myname + "!")
                                        callback.success();
                                    } else if (event === 'calling') {
                                        Janus.log("Waiting for the peer to answer...");
                                        self.callOnEvent('calling');
                                    } else if (event === 'incomingcall') {
                                        Janus.log("Incoming call from " + result["username"] + "!");
                                        self.peername = result["username"];
                                        self.jsep.answer = jsep;
                                        self.ringing(true);
                                        self.callOnEvent('incomingcall', self.peername);
                                    } else if (event === 'accepted') {
                                        var peer = result["username"];
                                        if (peer === null || peer === undefined) {
                                            Janus.log("Call started!");
                                        } else {
                                            Janus.log(peer + " accepted the call!");
                                            self.peername = peer;
                                        }
                                        if (jsep)
                                            self.plugin.handleRemoteJsep({ jsep: jsep });
                                        self.ringing(false);
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
                                    } else if (event === 'hangup') {
                                        Janus.log("Call hung up by " + result["username"] + " (" + result["reason"] + ")!");
                                       // self.plugin.hangup();
                                        self.ringing(false);
                                        self.callOnEvent('hangup', result["username"]);
                                    }
                                    else if (event === 'stop') {
                                        Janus.log("Result: " + result["start_time"] + ", " + result["stop_time"] + ", " + result["record_path"] + ", " + result["call_state"]);
                                        self.plugin.hangup();
                                        self.callOnEvent('stop', result["call_state"]);
                                    }
                                    else if (event === "timeout") {
                                        self.hangup();
                                        self.ringing(false);
                                        Janus.log("The call timeout. Hangup by user " + result["username"]);
                                    }
                                }
                            } else {
                                // FIXME Error?
                                var error = msg["error"];
                                bootbox.alert(error + " test");
                                if (error.indexOf("already taken") > 0) {
                                    // FIXME Use status codes...
                                    callback.error("Username has already taken");
                                }
                                // TODO Reset status
                                self.plugin.hangup();
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
    if (options.stream) {
        console.log("Local stream: " + options.stream);
    }
    this.plugin.createOffer(
        {
            media: { data: false },
            stream: options.stream ? options.stream : null,
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
        clearInterval(internalID);
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
    var hangup = { "request": "hangup", "reason": "decline"};
    this.plugin.send({ "message": hangup });
}

// hangup a call
VideoCall.prototype.hangup = function () {
    var hangup = { "request": "hangup"};
    this.plugin.send({ "message": hangup });
    //this.plugin.hangup();
}



