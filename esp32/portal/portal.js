(() => {
  // Pages ship in English. French applies when the browser's first language
  // is French, or when the visitor picks it; the choice is kept when the
  // captive browser allows storage.
  const STRINGS = {
    en: {
      "lang.label": "Language",
      "title.wifi": "Choose a network — Wignet",
      "title.connect": "Connect to the network — Wignet",
      "title.saved": "Settings saved — Wignet",
      "wifi.title": "Choose a network",
      "wifi.searching": "Searching…",
      "wifi.none": "No networks found",
      "wifi.scanFailed": "Search failed. Try again.",
      "wifi.hint": "Select the network your frame should connect to. It remembers up to 5 networks and joins whichever is in range.",
      "wifi.available": "Available networks",
      "wifi.nearby": "Nearby",
      "wifi.refresh": "↻ Refresh",
      "wifi.refreshAria": "Search again",
      "wifi.other": "Another network",
      "wifi.otherHint": "Enter its name manually",
      "wifi.help": "Can’t see your network? Check that it broadcasts on 2.4 GHz, then search again.",
      "network.secure": "Secured network",
      "network.open": "Open network",
      "connect.back": "‹ Back",
      "connect.backAria": "Back to networks",
      "connect.title": "Connect",
      "connect.selected": "Selected network",
      "connect.addTitle": "Add a network",
      "connect.ssidLabel": "Network name",
      "connect.ssidPlaceholder": "Wi‑Fi name",
      "connect.passLabel": "Password",
      "connect.passPlaceholder": "Wi‑Fi password",
      "connect.show": "Show",
      "connect.hide": "Hide",
      "connect.privacy": "Saved only on your Wignet.",
      "connect.submit": "Save and connect",
      "connect.saving": "Connecting…",
      "connect.retry": "Retry",
      "connect.errorSecure": "Connection interrupted. Enter the password again if the Wignet network is still visible.",
      "connect.errorOpen": "Connection interrupted. Try again if the Wignet network is still visible.",
      "saved.title": "Wi‑Fi saved",
      "saved.connecting": "Connecting",
      "saved.joining": "Your Wignet is now trying to join <strong>{ssid}</strong>.",
      "saved.statement": "Settings saved",
      "saved.next": "Next step",
      "saved.close": "You can close this page",
      "saved.apGone": "The <strong>{ap}</strong> network will disappear. The first image will appear in a few moments.",
    },
    fr: {
      "lang.label": "Langue",
      "title.wifi": "Choisir un réseau — Wignet",
      "title.connect": "Connexion au réseau — Wignet",
      "title.saved": "Configuration enregistrée — Wignet",
      "wifi.title": "Choisir un réseau",
      "wifi.searching": "Recherche en cours…",
      "wifi.none": "Aucun réseau trouvé",
      "wifi.scanFailed": "La recherche a échoué. Réessayez.",
      "wifi.hint": "Sélectionnez le réseau auquel votre cadre doit se connecter. Il retient jusqu’à 5 réseaux et rejoint celui qui est à portée.",
      "wifi.available": "Réseaux disponibles",
      "wifi.nearby": "À proximité",
      "wifi.refresh": "↻ Actualiser",
      "wifi.refreshAria": "Rechercher à nouveau",
      "wifi.other": "Un autre réseau",
      "wifi.otherHint": "Saisir son nom manuellement",
      "wifi.help": "Vous ne voyez pas votre réseau ? Vérifiez qu’il émet en 2,4 GHz, puis relancez la recherche.",
      "network.secure": "Réseau sécurisé",
      "network.open": "Réseau ouvert",
      "connect.back": "‹ Retour",
      "connect.backAria": "Retour aux réseaux",
      "connect.title": "Se connecter",
      "connect.selected": "Réseau sélectionné",
      "connect.addTitle": "Ajouter un réseau",
      "connect.ssidLabel": "Nom du réseau",
      "connect.ssidPlaceholder": "Nom du Wi‑Fi",
      "connect.passLabel": "Mot de passe",
      "connect.passPlaceholder": "Mot de passe du Wi‑Fi",
      "connect.show": "Afficher",
      "connect.hide": "Masquer",
      "connect.privacy": "Enregistré uniquement dans votre Wignet.",
      "connect.submit": "Enregistrer et connecter",
      "connect.saving": "Connexion en cours…",
      "connect.retry": "Réessayer",
      "connect.errorSecure": "Connexion interrompue. Saisissez à nouveau le mot de passe si le réseau Wignet est encore visible.",
      "connect.errorOpen": "Connexion interrompue. Réessayez si le réseau Wignet est encore visible.",
      "saved.title": "Wi‑Fi enregistré",
      "saved.connecting": "Connexion en cours",
      "saved.joining": "Votre Wignet essaie maintenant de rejoindre <strong>{ssid}</strong>.",
      "saved.statement": "Configuration enregistrée",
      "saved.next": "Prochaine étape",
      "saved.close": "Vous pouvez fermer cette page",
      "saved.apGone": "Le réseau <strong>{ap}</strong> va disparaître. Le premier affichage arrivera dans quelques instants.",
    },
  };
  const STORAGE_KEY = "wignet-portal-lang";

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  }

  function fill(template, vars, escape) {
    return template.replace(/\{(\w+)\}/g, (match, name) =>
      name in vars ? (escape ? escapeHtml(vars[name]) : vars[name]) : match);
  }

  function initialLang() {
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (saved in STRINGS) return saved;
    } catch { /* captive browsers may block storage */ }
    const first = (navigator.languages && navigator.languages[0]) || navigator.language || "en";
    return /^fr\b/i.test(first) ? "fr" : "en";
  }

  let lang = initialLang();
  const t = (key) => (key in STRINGS[lang] ? STRINGS[lang][key] : key in STRINGS.en ? STRINGS.en[key] : key);

  function applyLang() {
    document.documentElement.lang = lang;
    document.querySelectorAll("[data-i18n]").forEach((el) => {
      el.textContent = fill(t(el.dataset.i18n), el.dataset, false);
    });
    document.querySelectorAll("[data-i18n-html]").forEach((el) => {
      el.innerHTML = fill(t(el.dataset.i18nHtml), el.dataset, true);
    });
    document.querySelectorAll("[data-i18n-attr]").forEach((el) => {
      el.dataset.i18nAttr.split(",").forEach((pair) => {
        const [attr, key] = pair.split(":");
        el.setAttribute(attr, t(key));
      });
    });
  }

  const picker = document.querySelector("#lang");
  if (picker) {
    picker.value = lang;
    picker.addEventListener("change", () => {
      lang = picker.value in STRINGS ? picker.value : "en";
      try { localStorage.setItem(STORAGE_KEY, lang); } catch { /* not persisted */ }
      applyLang();
      refreshDynamicText();
    });
  }
  applyLang();

  // Text the script itself changes; re-applied when the language changes.
  let refreshDynamicText = () => {};

  function getJson(url) {
    return fetch(url, { cache: "no-store" }).then((response) => {
      if (!response.ok) throw new Error(url);
      return response.json();
    });
  }

  // The device's own access point name (Wignet-XXXX), for the saved view.
  // Optional: the page reads fine without it.
  if (document.querySelector("#saved-ap") && window.fetch) getJson("/device.json").then((device) => {
    if (!device || !device.ap) return;
    const savedAp = document.querySelector("#saved-ap");
    if (savedAp) savedAp.dataset.ap = device.ap;
    applyLang();
  }).catch(() => {});

  if (document.body.dataset.page === "wifi") setUpNetworkList();
  if (document.body.dataset.page === "connect") setUpConnect();

  function signalLevel(rssi) {
    // WiFiManager's RSSI-to-quality mapping: -100 dBm = 0 %, -50 dBm = 100 %.
    const quality = Math.max(0, Math.min(100, 2 * (rssi + 100)));
    return { quality, level: quality >= 75 ? 3 : quality >= 45 ? 2 : 1 };
  }

  function setUpNetworkList() {
    const list = document.querySelector("#network-list");
    const other = list.querySelector(".network-other");
    const status = document.querySelector("#network-status");
    const refresh = document.querySelector("#refresh");

    // Shown only while scanning or when there is no list to show.
    function setStatus(key) {
      status.hidden = !key;
      if (!key) return;
      status.dataset.i18n = key;
      status.textContent = t(key);
    }

    function render(networks) {
      list.querySelectorAll(".network:not(.network-other)").forEach((row) => row.remove());
      networks.forEach((network) => {
        const { quality, level } = signalLevel(network.rssi);
        const row = document.createElement("a");
        row.className = "network";
        row.href = "/connect?" + new URLSearchParams({ ssid: network.ssid, secure: network.secure ? "1" : "0" });
        row.innerHTML = `<span class="signal signal-${level}" aria-label="Signal ${quality}%"><i></i><i></i><i></i></span>` +
          `<span class="network-copy"><strong></strong><small data-i18n="${network.secure ? "network.secure" : "network.open"}"></small></span>`;
        row.querySelector("strong").textContent = network.ssid;
        list.insertBefore(row, other);
      });
      setStatus(networks.length ? null : "wifi.none");
      applyLang();
    }

    function load(force) {
      list.setAttribute("aria-busy", "true");
      refresh.classList.add("is-loading");
      refresh.disabled = true;
      if (force) setStatus("wifi.searching");
      return getJson(force ? "/networks.json?refresh=1" : "/networks.json")
        .then((data) => render(Array.isArray(data.networks) ? data.networks : []))
        .catch(() => setStatus("wifi.scanFailed"))
        .finally(() => {
          list.setAttribute("aria-busy", "false");
          refresh.classList.remove("is-loading");
          refresh.disabled = false;
          refresh.setAttribute("aria-label", t("wifi.refreshAria"));
        });
    }

    // The connect page comes from the Wignet itself, slowly: show at once
    // which network was tapped, and that it is on its way.
    list.addEventListener("click", (event) => {
      const row = event.target.closest(".network");
      if (!row || list.querySelector(".network.is-loading")) return;
      row.classList.add("is-loading");
      list.setAttribute("aria-busy", "true");
    });
    // Coming back with the browser's back button shows the page as it was.
    window.addEventListener("pageshow", () => {
      list.querySelectorAll(".network.is-loading").forEach((row) => row.classList.remove("is-loading"));
      list.setAttribute("aria-busy", "false");
    });

    refresh.addEventListener("click", () => load(true));
    if (window.fetch) load(false);
    else setStatus("wifi.scanFailed");
  }

  function setUpConnect() {
    const query = new URLSearchParams(location.search);
    const manual = query.get("manual") === "1";
    const chosen = (query.get("ssid") || "").slice(0, 32);
    if (!manual && !chosen) { location.replace("/wifi"); return; }
    const secure = manual || query.get("secure") !== "0";

    const form = document.querySelector("#connect-form");
    const ssid = document.querySelector("#s");
    const password = document.querySelector("#p");
    const heading = document.querySelector("#credentials-heading");
    const submit = document.querySelector(".submit-button");
    const submitLabel = submit.querySelector(".submit-label");
    const showPassword = document.querySelector(".show-password");
    let submitKey = "connect.submit";
    let errorShown = false;

    ssid.value = chosen;
    document.querySelector("#ssid-field").hidden = !manual;
    document.querySelector("#password-field").hidden = !secure;
    document.querySelector("#privacy").hidden = !secure;
    if (manual) heading.dataset.i18n = "connect.addTitle";
    else heading.textContent = chosen;
    applyLang();

    refreshDynamicText = () => {
      submitLabel.textContent = t(submitKey);
      showPassword.textContent = t(password.type === "text" ? "connect.hide" : "connect.show");
      const error = document.querySelector("#save-error");
      if (error && errorShown) error.textContent = t(secure ? "connect.errorSecure" : "connect.errorOpen");
    };

    function updateSubmit() {
      submit.disabled = !ssid.value.trim() || (secure && !password.value);
    }

    ssid.addEventListener("input", updateSubmit);
    password.addEventListener("input", updateSubmit);

    showPassword.addEventListener("click", () => {
      const showing = showPassword.getAttribute("aria-pressed") === "true";
      password.type = showing ? "password" : "text";
      showPassword.textContent = t(showing ? "connect.show" : "connect.hide");
      showPassword.setAttribute("aria-pressed", String(!showing));
      password.focus();
    });

    function showSaved(name) {
      document.querySelector("#connect-view").hidden = true;
      document.querySelector("#saved-view").hidden = false;
      document.querySelector("#saved-ssid").textContent = name;
      document.querySelector("#saved-joining").dataset.ssid = name;
      document.title = t("title.saved");
      document.querySelector("title").dataset.i18n = "title.saved";
      applyLang();
      window.scrollTo(0, 0);
    }

    // A website form submission can trigger a "remember password" prompt.
    // Send the Wi-Fi key to the same endpoint without submitting the form.
    // The field is cleared while sending; after a failure a secured network
    // needs the password typed again before Retry is enabled.
    async function saveWifi() {
      if (submit.disabled) return;
      const body = new URLSearchParams(new FormData(form));
      const name = ssid.value.trim();
      password.value = "";
      if (password.type === "text") password.type = "password";
      showPassword.setAttribute("aria-pressed", "false");
      submit.disabled = true;
      submitKey = "connect.saving";
      submitLabel.textContent = t(submitKey);
      try {
        const response = await fetch(form.action, { method: "POST", body });
        if (!response.ok) throw new Error("Save failed");
        showSaved(name);
      } catch {
        submitKey = "connect.retry";
        const error = document.querySelector("#save-error") || document.createElement("p");
        error.id = "save-error";
        error.className = "hint";
        errorShown = true;
        submit.after(error);
        refreshDynamicText();
        updateSubmit();
      }
    }

    if (window.fetch && window.FormData && window.URLSearchParams) {
      submit.type = "button";
      submit.addEventListener("click", saveWifi);
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        saveWifi();
      });
      form.addEventListener("keydown", (event) => {
        if (event.key === "Enter" && event.target.tagName === "INPUT") {
          event.preventDefault();
          saveWifi();
        }
      });
    }

    updateSubmit();
  }
})();
