/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

const { classes: Cc, interfaces: Ci, utils: Cu, results: Cr } = Components;

let { Services } = Cu.import("resource://gre/modules/Services.jsm", {});

// Disable logging for faster test runs. Set this pref to true if you want to
// debug a test in your try runs. Both the debugger server and frontend will
// be affected by this pref.
let gEnableLogging = Services.prefs.getBoolPref("devtools.debugger.log");
Services.prefs.setBoolPref("devtools.debugger.log", false);

let { Task } = Cu.import("resource://gre/modules/Task.jsm", {});
let { Promise: promise } = Cu.import("resource://gre/modules/commonjs/sdk/core/promise.js", {});
let { gDevTools } = Cu.import("resource:///modules/devtools/gDevTools.jsm", {});
let { devtools } = Cu.import("resource://gre/modules/devtools/Loader.jsm", {});
let { DevToolsUtils } = Cu.import("resource://gre/modules/devtools/DevToolsUtils.jsm", {});
let { BrowserDebuggerProcess } = Cu.import("resource:///modules/devtools/DebuggerProcess.jsm", {});
let { DebuggerServer } = Cu.import("resource://gre/modules/devtools/dbg-server.jsm", {});
let { DebuggerClient } = Cu.import("resource://gre/modules/devtools/dbg-client.jsm", {});
let { SourceEditor } = Cu.import("resource:///modules/devtools/sourceeditor/source-editor.jsm", {});
let { AddonManager } = Cu.import("resource://gre/modules/AddonManager.jsm", {});
let TargetFactory = devtools.TargetFactory;
let Toolbox = devtools.Toolbox;

const EXAMPLE_URL = "http://example.com/browser/browser/devtools/debugger/test/";

// All tests are asynchronous.
waitForExplicitFinish();

registerCleanupFunction(function() {
  info("finish() was called, cleaning up...");
  Services.prefs.setBoolPref("devtools.debugger.log", gEnableLogging);

  // Properly shut down the server to avoid memory leaks.
  DebuggerServer.destroy();
});

// Import the GCLI test helper
let testDir = gTestPath.substr(0, gTestPath.lastIndexOf("/"));
Services.scriptloader.loadSubScript(testDir + "../../../commandline/test/helpers.js", this);

// Redeclare dbg_assert with a fatal behavior.
function dbg_assert(cond, e) {
  if (!cond) {
    throw e;
  }
}

function addWindow(aUrl) {
  info("Adding window: " + aUrl);
  return promise.resolve(getDOMWindow(window.open(aUrl)));
}

function getDOMWindow(aReference) {
  return aReference
    .QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIWebNavigation)
    .QueryInterface(Ci.nsIDocShellTreeItem).rootTreeItem
    .QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindow);
}

function addTab(aUrl, aWindow) {
  info("Adding tab: " + aUrl);

  let deferred = promise.defer();
  let targetWindow = aWindow || window;
  let targetBrowser = targetWindow.gBrowser;

  targetWindow.focus();
  let tab = targetBrowser.selectedTab = targetBrowser.addTab(aUrl);
  let linkedBrowser = tab.linkedBrowser;

  linkedBrowser.addEventListener("load", function onLoad() {
    linkedBrowser.removeEventListener("load", onLoad, true);
    info("Tab added and finished loading: " + aUrl);
    deferred.resolve(tab);
  }, true);

  return deferred.promise;
}

function removeTab(aTab, aWindow) {
  info("Removing tab.");

  let deferred = promise.defer();
  let targetWindow = aWindow || window;
  let targetBrowser = targetWindow.gBrowser;
  let tabContainer = targetBrowser.tabContainer;

  tabContainer.addEventListener("TabClose", function onClose(aEvent) {
    tabContainer.removeEventListener("TabClose", onClose, false);
    info("Tab removed and finished closing.");
    deferred.resolve();
  }, false);

  targetBrowser.removeTab(aTab);
  return deferred.promise;
}

function addAddon(aUrl) {
  info("Installing addon: " + aUrl);

  let deferred = promise.defer();

  AddonManager.getInstallForURL(aUrl, aInstaller => {
    aInstaller.install();
    let listener = {
      onInstallEnded: function(aAddon, aAddonInstall) {
        aInstaller.removeListener(listener);
        deferred.resolve(aAddonInstall);
      }
    };
    aInstaller.addListener(listener);
  }, "application/x-xpinstall");

  return deferred.promise;
}

function removeAddon(aAddon) {
  info("Removing addon.");

  let deferred = promise.defer();

  let listener = {
    onUninstalled: function(aUninstalledAddon) {
      if (aUninstalledAddon != aAddon) {
        return;
      }
      AddonManager.removeAddonListener(listener);
      deferred.resolve();
    }
  };
  AddonManager.addAddonListener(listener);
  aAddon.uninstall();

  return deferred.promise;
}

function getTabActorForUrl(aClient, aUrl) {
  let deferred = promise.defer();

  aClient.listTabs(aResponse => {
    let tabActor = aResponse.tabs.filter(aGrip => aGrip.url == aUrl).pop();
    deferred.resolve(tabActor);
  });

  return deferred.promise;
}

function getAddonActorForUrl(aClient, aUrl) {
  let deferred = promise.defer();

  aClient.listAddons(aResponse => {
    let addonActor = aResponse.addons.filter(aGrip => aGrip.url == aUrl).pop();
    deferred.resolve(addonActor);
  });

  return deferred.promise;
}

function attachTabActorForUrl(aClient, aUrl) {
  let deferred = promise.defer();

  getTabActorForUrl(aClient, aUrl).then(aGrip => {
    aClient.attachTab(aGrip.actor, aResponse => {
      deferred.resolve([aGrip, aResponse]);
    });
  });

  return deferred.promise;
}

function attachThreadActorForUrl(aClient, aUrl) {
  let deferred = promise.defer();

  attachTabActorForUrl(aClient, aUrl).then(([aGrip, aResponse]) => {
    aClient.attachThread(aResponse.threadActor, (aResponse, aThreadClient) => {
      aThreadClient.resume(aResponse => {
        deferred.resolve(aThreadClient);
      });
    });
  });

  return deferred.promise;
}

function once(aTarget, aEventName, aUseCapture = false) {
  info("Waiting for event: '" + aEventName + "' on " + aTarget + ".");

  let deferred = promise.defer();

  for (let [add, remove] of [
    ["addEventListener", "removeEventListener"],
    ["addListener", "removeListener"],
    ["on", "off"]
  ]) {
    if ((add in aTarget) && (remove in aTarget)) {
      aTarget[add](aEventName, function onEvent(...aArgs) {
        aTarget[remove](aEventName, onEvent, aUseCapture);
        deferred.resolve.apply(deferred, aArgs);
      }, aUseCapture);
      break;
    }
  }

  return deferred.promise;
}

function waitForTick() {
  let deferred = promise.defer();
  executeSoon(deferred.resolve);
  return deferred.promise;
}

function waitForTime(aDelay) {
  let deferred = promise.defer();
  setTimeout(deferred.resolve, aDelay);
  return deferred.promise;
}

function waitForSourceShown(aPanel, aUrl) {
  return waitForDebuggerEvents(aPanel, aPanel.panelWin.EVENTS.SOURCE_SHOWN).then(aSource => {
    let sourceUrl = aSource.url;
    info("Source shown: " + sourceUrl);

    if (!sourceUrl.contains(aUrl)) {
      return waitForSourceShown(aPanel, aUrl);
    } else {
      ok(true, "The correct source has been shown.");
    }
  });
}

function ensureSourceIs(aPanel, aUrl, aWaitFlag = false) {
  if (aPanel.panelWin.DebuggerView.Sources.selectedValue.contains(aUrl)) {
    ok(true, "Expected source is shown: " + aUrl);
    return promise.resolve(null);
  }
  if (aWaitFlag) {
    return waitForSourceShown(aPanel, aUrl);
  }
  ok(false, "Expected source was not already shown: " + aUrl);
  return promise.reject(null);
}

function waitForCaretUpdated(aPanel, aLine, aCol = 1) {
  return waitForEditorEvents(aPanel, SourceEditor.EVENTS.SELECTION).then(() => {
    let caret = aPanel.panelWin.DebuggerView.editor.getCaretPosition();
    info("Caret updated: " + (caret.line + 1) + ", " + (caret.col + 1));

    if (!isCaretPos(aPanel, aLine, aCol)) {
      return waitForCaretUpdated(aPanel, aLine, aCol);
    } else {
      ok(true, "The correct caret position has been set.");
    }
  });
}

function ensureCaretAt(aPanel, aLine, aCol = 1, aWaitFlag = false) {
  if (isCaretPos(aPanel, aLine, aCol)) {
    ok(true, "Expected caret position is set: " + aLine + "," + aCol);
    return promise.resolve(null);
  }
  if (aWaitFlag) {
    return waitForCaretUpdated(aPanel, aLine, aCol);
  }
  ok(false, "Expected caret position was not already set: " + aLine + "," + aCol);
  return promise.reject(null);
}

function isCaretPos(aPanel, aLine, aCol = 1) {
  let editor = aPanel.panelWin.DebuggerView.editor;
  let caret = editor.getCaretPosition();

  // Source editor starts counting line and column numbers from 0.
  info("Current editor caret position: " + (caret.line + 1) + ", " + (caret.col + 1));
  return caret.line == (aLine - 1) && caret.col == (aCol - 1);
}

function isEditorSel(aPanel, [start, end]) {
  let editor = aPanel.panelWin.DebuggerView.editor;
  let range = editor.getSelection();

  // Source editor starts counting line and column numbers from 0.
  info("Current editor selection: " + (range.start + 1) + ", " + (range.end + 1));
  return range.start == (start - 1) && range.end == (end - 1);
}

function waitForSourceAndCaret(aPanel, aUrl, aLine, aCol) {
  return promise.all([
    waitForSourceShown(aPanel, aUrl),
    waitForCaretUpdated(aPanel, aLine, aCol)
  ]);
}

function waitForCaretAndScopes(aPanel, aLine, aCol) {
  return promise.all([
    waitForCaretUpdated(aPanel, aLine, aCol),
    waitForDebuggerEvents(aPanel, aPanel.panelWin.EVENTS.FETCHED_SCOPES)
  ]);
}

function waitForSourceAndCaretAndScopes(aPanel, aUrl, aLine, aCol) {
  return promise.all([
    waitForSourceAndCaret(aPanel, aUrl, aLine, aCol),
    waitForDebuggerEvents(aPanel, aPanel.panelWin.EVENTS.FETCHED_SCOPES)
  ]);
}

function waitForDebuggerEvents(aPanel, aEventName, aEventRepeat = 1) {
  info("Waiting for debugger event: '" + aEventName + "' to fire: " + aEventRepeat + " time(s).");

  let deferred = promise.defer();
  let panelWin = aPanel.panelWin;
  let count = 0;

  panelWin.on(aEventName, function onEvent(aEventName, ...aArgs) {
    info("Debugger event '" + aEventName + "' fired: " + (++count) + " time(s).");

    if (count == aEventRepeat) {
      ok(true, "Enough '" + aEventName + "' panel events have been fired.");
      panelWin.off(aEventName, onEvent);
      deferred.resolve.apply(deferred, aArgs);
    }
  });

  return deferred.promise;
}

function waitForEditorEvents(aPanel, aEventName, aEventRepeat = 1) {
  info("Waiting for editor event: '" + aEventName + "' to fire: " + aEventRepeat + " time(s).");

  let deferred = promise.defer();
  let editor = aPanel.panelWin.DebuggerView.editor;
  let count = 0;

  editor.addEventListener(aEventName, function onEvent(...aArgs) {
    info("Editor event '" + aEventName + "' fired: " + (++count) + " time(s).");

    if (count == aEventRepeat) {
      ok(true, "Enough '" + aEventName + "' editor events have been fired.");
      editor.removeEventListener(aEventName, onEvent);
      deferred.resolve.apply(deferred, aArgs);
    }
  });

  return deferred.promise;
}

function waitForThreadEvents(aPanel, aEventName, aEventRepeat = 1) {
  info("Waiting for thread event: '" + aEventName + "' to fire: " + aEventRepeat + " time(s).");

  let deferred = promise.defer();
  let thread = aPanel.panelWin.gThreadClient;
  let count = 0;

  thread.addListener(aEventName, function onEvent(aEventName, ...aArgs) {
    info("Thread event '" + aEventName + "' fired: " + (++count) + " time(s).");

    if (count == aEventRepeat) {
      ok(true, "Enough '" + aEventName + "' thread events have been fired.");
      thread.removeListener(aEventName, onEvent);
      deferred.resolve.apply(deferred, aArgs);
    }
  });

  return deferred.promise;
}

function ensureThreadClientState(aPanel, aState) {
  let thread = aPanel.panelWin.gThreadClient;
  let state = thread.state;

  info("Thread is: '" + state + "'.");

  if (state == aState) {
    return promise.resolve(null);
  } else {
    return waitForThreadEvents(aPanel, aState);
  }
}

function navigateActiveTabTo(aPanel, aUrl, aWaitForEventName, aEventRepeat) {
  let finished = waitForDebuggerEvents(aPanel, aWaitForEventName, aEventRepeat);
  let activeTab = aPanel.panelWin.gClient.activeTab;
  aUrl ? activeTab.navigateTo(aUrl) : activeTab.reload();
  return finished;
}

function navigateActiveTabInHistory(aPanel, aDirection, aWaitForEventName, aEventRepeat) {
  let finished = waitForDebuggerEvents(aPanel, aWaitForEventName, aEventRepeat);
  content.history[aDirection]();
  return finished;
}

function reloadActiveTab(aPanel, aWaitForEventName, aEventRepeat) {
  return navigateActiveTabTo(aPanel, null, aWaitForEventName, aEventRepeat);
}

function clearText(aElement) {
  info("Clearing text...");
  aElement.focus();
  aElement.value = "";
}

function setText(aElement, aText) {
  clearText(aElement);
  info("Setting text: " + aText);
  aElement.value = aText;
}

function typeText(aElement, aText) {
  info("Typing text: " + aText);
  aElement.focus();
  EventUtils.sendString(aText, aElement.ownerDocument.defaultView);
}

function backspaceText(aElement, aTimes) {
  info("Pressing backspace " + aTimes + " times.");
  for (let i = 0; i < aTimes; i++) {
    aElement.focus();
    EventUtils.sendKey("BACK_SPACE", aElement.ownerDocument.defaultView);
  }
}

function getTab(aTarget, aWindow) {
  if (aTarget instanceof XULElement) {
    return promise.resolve(aTarget);
  } else {
    return addTab(aTarget, aWindow);
  }
}

function initDebugger(aTarget, aWindow) {
  info("Initializing a debugger panel.");

  return getTab(aTarget, aWindow).then(aTab => {
    info("Debugee tab added successfully: " + aTarget);

    let deferred = promise.defer();
    let debuggee = aTab.linkedBrowser.contentWindow.wrappedJSObject;
    let target = TargetFactory.forTab(aTab);

    gDevTools.showToolbox(target, "jsdebugger").then(aToolbox => {
      info("Debugger panel shown successfully.");

      let debuggerPanel = aToolbox.getCurrentPanel();
      let panelWin = debuggerPanel.panelWin;

      // Wait for the initial resume...
      panelWin.gClient.addOneTimeListener("resumed", () => {
        info("Debugger client resumed successfully.");

        prepareDebugger(debuggerPanel);
        deferred.resolve([aTab, debuggee, debuggerPanel, aWindow]);
      });
    });

    return deferred.promise;
  });
}

function initChromeDebugger(aOnClose) {
  info("Initializing a chrome debugger process.");

  let deferred = promise.defer();

  // Wait for the debugger process to start...
  BrowserDebuggerProcess.init(aOnClose, aProcess => {
    info("Chrome debugger process started successfully.");

    prepareDebugger(aProcess);
    deferred.resolve(aProcess);
  });

  return deferred.promise;
}

function prepareDebugger(aDebugger) {
  if ("target" in aDebugger) {
    let variables = aDebugger.panelWin.DebuggerView.Variables;
    variables.lazyEmpty = false;
    variables.lazyAppend = false;
    variables.lazyExpand = false;
    variables.lazySearch = false;
  } else {
    // Nothing to do here yet.
  }
}

function teardown(aPanel, aFlags = {}) {
  info("Destroying the specified debugger.");

  let toolbox = aPanel._toolbox;
  let tab = aPanel.target.tab;
  let debuggerRootActorDisconnected = once(window, "Debugger:Shutdown");
  let debuggerPanelDestroyed = once(aPanel, "destroyed");
  let devtoolsToolboxDestroyed = toolbox.destroy();

  return promise.all([
    debuggerRootActorDisconnected,
    debuggerPanelDestroyed,
    devtoolsToolboxDestroyed
  ]).then(() => aFlags.noTabRemoval ? null : removeTab(tab));
}

function closeDebuggerAndFinish(aPanel, aFlags = {}) {
  let thread = aPanel.panelWin.gThreadClient;
  if (thread.state == "paused" && !aFlags.whilePaused) {
    ok(false, "You should use 'resumeDebuggerThenCloseAndFinish' instead, " +
              "unless you're absolutely sure about what you're doing.");
  }
  return teardown(aPanel, aFlags).then(finish);
}

function resumeDebuggerThenCloseAndFinish(aPanel, aFlags = {}) {
  let deferred = promise.defer();
  let thread = aPanel.panelWin.gThreadClient;
  thread.resume(() => closeDebuggerAndFinish(aPanel, aFlags).then(deferred.resolve));
  return deferred.promise;
}
