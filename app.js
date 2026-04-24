const runtimeConfig = window.__MIA_CONFIG__ || {};
const apiBaseUrl = runtimeConfig.apiBaseUrl || "http://127.0.0.1:8080";
const workspaceConfigUrl = `${apiBaseUrl}/api/state`;
const executeUrl = `${apiBaseUrl}/api/execute`;

const fileInput = document.getElementById("file-input");
const commandInput = document.getElementById("command-input");
const output = document.getElementById("command-output");
const artifactsList = document.getElementById("artifacts-list");
const apiStatus = document.getElementById("api-status");
const sessionStatus = document.getElementById("session-status");
const mountCount = document.getElementById("mount-count");

const exampleScript = `# Flujo base del proyecto
mkdisk -size=10 -path=disks/demo.mia
fdisk -size=4 -path=disks/demo.mia -name=primaria1
mount -path=disks/demo.mia -name=primaria1
mkfs -id=001A
login -user=root -pass=123 -id=001A
mkdir -p -path=/home/user/docs
mkfile -path=/home/user/docs/readme.txt -size=32
cat -file1=/home/user/docs/readme.txt`;

async function refreshState() {
  try {
    const response = await fetch(workspaceConfigUrl);
    if (!response.ok) {
      throw new Error("API no disponible");
    }

    const data = await response.json();
    apiStatus.textContent = `Lista en ${data.port}`;
    sessionStatus.textContent = data.session.active
      ? `${data.session.user} @ ${data.session.partitionId}`
      : "Sin sesión";
    mountCount.textContent = String(data.mounts.length);
  } catch (error) {
    apiStatus.textContent = "Desconectada";
    sessionStatus.textContent = "Sin sesión";
    mountCount.textContent = "0";
  }
}

function renderArtifacts(artifacts) {
  if (!artifacts.length) {
    artifactsList.innerHTML = `<p class="empty-state">Todavía no hay reportes generados.</p>`;
    return;
  }

  artifactsList.innerHTML = artifacts
    .map(
      (artifact) => `
        <div class="artifact-item">
          <span>${artifact}</span>
        </div>
      `
    )
    .join("");
}

async function runCommands() {
  const commands = commandInput.value;
  output.textContent = "Ejecutando...";

  try {
    const response = await fetch(executeUrl, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify({ commands })
    });

    const data = await response.json();
    output.textContent = data.output.join("\n");
    renderArtifacts(data.artifacts || []);
    await refreshState();
  } catch (error) {
    output.textContent = `Error al ejecutar comandos: ${error.message}`;
  }
}

document.getElementById("run-button").addEventListener("click", runCommands);
document.getElementById("example-button").addEventListener("click", () => {
  commandInput.value = exampleScript;
});
document.getElementById("clear-output").addEventListener("click", () => {
  output.textContent = "";
});

fileInput.addEventListener("change", async (event) => {
  const file = event.target.files?.[0];
  if (!file) {
    return;
  }

  const content = await file.text();
  commandInput.value = content;
});

refreshState();
setInterval(refreshState, 4000);
