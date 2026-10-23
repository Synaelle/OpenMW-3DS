// The setup app's page: the folders, the progress, the steps to finish. Everything it knows comes from the app's
// commands (app/src/main.rs).

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);

const app = {
  version: "",
  morrowindOk: false,
  outputFits: true,
  installed: null,
  output: "",
};

function show(view) {
  for (const section of document.querySelectorAll(".view")) {
    section.hidden = section.id !== view;
  }
}

function setStatus(element, text, problem = false) {
  element.textContent = text;
  element.classList.toggle("problem", problem);
}

// "Tribunal and Bloodmoon", "A, B and C".
function list(items) {
  return items.length < 2 ? items.join("") : `${items.slice(0, -1).join(", ")} and ${items[items.length - 1]}`;
}

// Runs `task` once typing has paused, the latest call only.
function debounced(task, delay = 300) {
  let timer = 0;
  return () => {
    clearTimeout(timer);
    timer = setTimeout(task, delay);
  };
}

let morrowindCheck = 0;
async function checkMorrowind() {
  const ticket = ++morrowindCheck;
  const info = await invoke("inspect_morrowind", { path: $("morrowind").value });
  if (ticket !== morrowindCheck) return;
  app.morrowindOk = info.problems.length === 0;
  if (!app.morrowindOk) {
    setStatus($("morrowind-status"), info.problems[0], true);
  } else {
    let text = `The ${info.language} Morrowind is here.`;
    if (info.expansions.length) {
      const plural = info.expansions.length > 1;
      text += ` ${list(info.expansions)} ${plural ? "are" : "is"} there too: ${plural ? "they come" : "it comes"} in a later version.`;
    }
    setStatus($("morrowind-status"), text);
  }
  checkOutput();
}

let outputCheck = 0;
async function checkOutput() {
  const ticket = ++outputCheck;
  const output = $("output").value.trim();
  if (!output) {
    app.installed = null;
    setStatus($("output-status"), "Choose where to put the files.", true);
    updatePrepareButton();
    return;
  }
  const info = await invoke("inspect_output", { morrowind: $("morrowind").value, output });
  if (ticket !== outputCheck) return;
  app.installed = info.installed;
  app.outputFits = info.fits;
  let text = info.installed ? `OpenMW ${info.installed} is installed here: only what changed will be prepared. ` : "";
  if (!app.morrowindOk) {
    setStatus($("output-status"), text.trim());
  } else if (!info.fits) {
    setStatus(
      $("output-status"),
      `Not enough space here: preparing writes about ${info.to_write}, and ${info.available} is free.`,
      true,
    );
  } else {
    text += `The install takes about ${info.install_size} on your SD card.`;
    setStatus($("output-status"), text);
  }
  updatePrepareButton();
}

function updatePrepareButton() {
  $("prepare").textContent = app.installed ? "Update" : "Prepare";
}

async function browse(input, title, mapPicked) {
  const picked = await invoke("pick_folder", { title, start: input.value });
  if (!picked) return;
  input.value = mapPicked ? await invoke("output_for", { picked }) : picked;
  input.dispatchEvent(new Event("change"));
}

async function prepare() {
  setStatus($("notice"), "");
  if (!app.morrowindOk) {
    setStatus($("notice"), $("morrowind-status").textContent, true);
    return;
  }
  if (!$("output").value.trim()) {
    setStatus($("notice"), "Choose an output folder.", true);
    return;
  }
  if (!app.outputFits) {
    setStatus($("notice"), $("output-status").textContent, true);
    return;
  }
  try {
    app.output = await invoke("start_prepare", { morrowind: $("morrowind").value, output: $("output").value });
  } catch (message) {
    setStatus($("notice"), String(message), true);
    return;
  }
  $("output").value = app.output;
  $("cancel").disabled = false;
  $("step").textContent = "Starting";
  $("bar-fill").style.width = "0";
  show("working");
}

function finished(result) {
  if (result.outcome === "cancelled") {
    show("ready");
    setStatus($("notice"), "Cancelled. Prepare again to finish the folder: what's already done is kept.", true);
    checkOutput();
    return;
  }
  if (result.outcome === "failed") {
    $("error").textContent = result.error;
    $("failed-log").textContent = result.log ? `The details are in ${result.log}.` : "";
    show("failed");
    return;
  }
  app.output = result.output;
  $("done-output").textContent = result.output;
  $("done-kept").textContent = result.skipped.length
    ? `Kept from the earlier install: ${list(result.skipped)}.`
    : "";
  $("done-copy").textContent = result.skipped.length
    ? "Copy its contents to your SD card's root, replacing the files there."
    : "Copy its contents to your SD card's root.";
  $("done-size").textContent = result.size ? `It takes ${result.size}.` : "";
  setStatus($("sd-status"), "");
  show("done");
}

async function checkSd() {
  const check = await invoke("check_sd", { output: app.output });
  if (!check) return;
  if (check.error) {
    setStatus($("sd-status"), check.error, true);
    return;
  }
  const wrong = check.missing.length + check.different.length;
  if (wrong === 0) {
    setStatus($("sd-status"), `All ${check.files.toLocaleString("en")} files are on the SD card.`);
  } else {
    const example = check.missing[0] ?? check.different[0];
    const files = wrong === 1 ? "1 file is" : `${wrong.toLocaleString("en")} files are`;
    setStatus(
      $("sd-status"),
      `${files} missing or incomplete on the SD card (${example}). Copy the folder again.`,
      true,
    );
  }
}

async function start() {
  const startup = await invoke("startup");
  app.version = startup.version;
  $("morrowind").value = startup.morrowind ?? "";
  $("output").value = startup.output;
  if (!startup.port_found) {
    setStatus($("notice"), 'The port\'s files are missing: keep the "port" folder next to this program.', true);
  }

  const morrowindChanged = debounced(checkMorrowind);
  const outputChanged = debounced(checkOutput);
  $("morrowind").addEventListener("input", morrowindChanged);
  $("morrowind").addEventListener("change", checkMorrowind);
  $("output").addEventListener("input", outputChanged);
  $("output").addEventListener("change", checkOutput);
  $("morrowind-browse").addEventListener("click", () => browse($("morrowind"), "Choose the Morrowind folder", false));
  $("output-browse").addEventListener("click", () => browse($("output"), "Choose where to put the files", true));
  $("prepare").addEventListener("click", prepare);
  $("cancel").addEventListener("click", () => {
    $("cancel").disabled = true;
    $("step").textContent = "Cancelling...";
    invoke("cancel");
  });
  $("back").addEventListener("click", () => show("ready"));
  $("open-folder").addEventListener("click", () => invoke("open_folder", { path: app.output }));
  $("close").addEventListener("click", () => window.close());
  $("check-sd").addEventListener("click", checkSd);
  for (const link of document.querySelectorAll("a[data-url]")) {
    link.addEventListener("click", (event) => {
      event.preventDefault();
      invoke("open_url", { url: link.dataset.url });
    });
  }

  await listen("progress", (event) => {
    if ($("cancel").disabled) return;
    const percent = Math.floor(event.payload.fraction * 100);
    $("step").textContent = `${event.payload.step}, ${percent}%`;
    $("bar-fill").style.width = `${event.payload.fraction * 100}%`;
  });
  await listen("finished", (event) => finished(event.payload));

  const qr = await invoke("cia_qr");
  if (qr) {
    $("qr-code").innerHTML = qr.svg;
    $("qr-code").title = qr.url;
    $("qr").hidden = false;
  }

  await checkMorrowind();
  invoke("shown");

  // Is there a newer release? Asked once in the background (no connection: nothing shown).
  const newer = await invoke("newer_version");
  if (newer) {
    $("newer-text").textContent = `Version ${newer} is out, this is ${app.version}.`;
    $("newer").hidden = false;
    const page = await invoke("releases_page");
    $("newer-link").addEventListener("click", (event) => {
      event.preventDefault();
      invoke("open_url", { url: page });
    });
  }
}

start();
