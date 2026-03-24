const express = require('express');
const path = require('path');
const fs = require('fs');
const yaml = require('js-yaml');
const { spawn, exec, execSync } = require('child_process');

const app = express();
const PORT = 3000;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

const CONFIG_DIR = path.join(__dirname, '..', 'config');
const BUILD_DIR = path.join(__dirname, '..', 'build');
const CONFIG_PATH = path.join(__dirname, '..', 'config', 'config.entities.yaml');

const CLUSTERS = {
  0: { name: 'us-east-1',      label: 'N. Virginia' },
  1: { name: 'us-west-2',      label: 'Oregon' },
  2: { name: 'eu-west-1',      label: 'Ireland' },
  3: { name: 'eu-central-1',   label: 'Frankfurt' },
  4: { name: 'ap-southeast-1', label: 'Singapore' },
  5: { name: 'ap-northeast-1', label: 'Tokyo' },
  6: { name: 'ap-south-1',     label: 'Mumbai' },
  7: { name: 'sa-east-1',      label: 'São Paulo' },
  8: { name: 'ap-southeast-2', label: 'Sydney' },
  9: { name: 'ca-central-1',   label: 'Canada' },
};

const LATENCY_MATRIX = [
  [  0,  62,  75,  89, 230, 158, 186, 118, 206,  16],
  [ 62,   0, 138, 150, 163, 108, 230, 180, 148,  56],
  [ 75, 138,   0,  25, 170, 220, 120, 178, 260,  78],
  [ 89, 150,  25,   0, 155, 230, 105, 196, 270,  96],
  [230, 163, 170, 155,   0,  68,  58, 320,  92, 225],
  [158, 108, 220, 230,  68,   0, 118, 270, 108, 155],
  [186, 230, 120, 105,  58, 118,   0, 290, 142, 200],
  [118, 180, 178, 196, 320, 270, 290,   0, 310, 130],
  [206, 148, 260, 270,  92, 108, 142, 310,   0, 210],
  [ 16,  56,  78,  96, 225, 155, 200, 130, 210,   0],
];

// Track running processes
let serverProcess = null;
let serverOutput = [];

// ──────────────────────────────────────────────────────────────
// GET /api/current-protocol — read runtime.selection.yaml
// ──────────────────────────────────────────────────────────────
app.get('/api/current-protocol', (req, res) => {
  try {
    const configPath = path.join(CONFIG_DIR, 'runtime.selection.yaml');
    const config = yaml.load(fs.readFileSync(configPath, 'utf8'));
    res.json({ protocol: config.protocol || 'PBFT' });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// POST /api/select-protocol — write to runtime.selection.yaml
// ──────────────────────────────────────────────────────────────
app.post('/api/select-protocol', (req, res) => {
  const { protocol } = req.body;
  if (!protocol) return res.status(400).json({ error: 'Missing protocol' });

  try {
    const configPath = path.join(CONFIG_DIR, 'runtime.selection.yaml');
    const config = { protocol };
    fs.writeFileSync(configPath, yaml.dump(config));
    console.log(`Protocol set to: ${protocol}`);
    res.json({ success: true, protocol });
  } catch (err) {
    console.error('Failed to set protocol:', err);
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// POST /api/save-config — save entities + delays to config.entities.yaml
// ──────────────────────────────────────────────────────────────
app.post('/api/save-config', (req, res) => {
  const { nodes, delays } = req.body;

  try {
    const configPath = path.join(CONFIG_DIR, 'config.entities.yaml');
    const config = yaml.load(fs.readFileSync(configPath, 'utf8')) || {};

    config.entities = nodes.map((node) => ({
      id: node.id,
      role: node.role,
      byzantine: false,
      peers: nodes.map((n) => n.id),
    }));

    config.delays = delays || [];

    fs.writeFileSync(configPath, yaml.dump(config));
    console.log(`Config saved: ${nodes.length} nodes, ${(delays || []).length} delays`);
    res.json({ success: true });
  } catch (err) {
    console.error('Failed to save config:', err);
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// POST /api/start-server — run ./CppBedrock in build/
// ──────────────────────────────────────────────────────────────
app.post('/api/start-server', (req, res) => {
  if (serverProcess) {
    return res.status(400).json({ error: 'Server already running' });
  }

  try {
    serverOutput = [];
    serverProcess = spawn('./CppBedrock', [], {
      cwd: BUILD_DIR,
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    serverProcess.stdout.on('data', (data) => {
      const lines = data.toString();
      serverOutput.push(lines);
      // Keep only last 500 lines
      if (serverOutput.length > 500) serverOutput.shift();
    });

    serverProcess.stderr.on('data', (data) => {
      const lines = data.toString();
      serverOutput.push(lines);
      if (serverOutput.length > 500) serverOutput.shift();
    });

    serverProcess.on('close', (code) => {
      console.log(`CppBedrock exited with code ${code}`);
      serverOutput.push(`[Process exited with code ${code}]\n`);
      serverProcess = null;
    });

    serverProcess.on('error', (err) => {
      console.error('Failed to start CppBedrock:', err);
      serverOutput.push(`[Error: ${err.message}]\n`);
      serverProcess = null;
    });

    console.log('CppBedrock started');
    res.json({ success: true });
  } catch (err) {
    console.error('Failed to start server:', err);
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// POST /api/stop-server — kill the CppBedrock process
// ──────────────────────────────────────────────────────────────
app.post('/api/stop-server', (req, res) => {
  if (!serverProcess) {
    // Try to kill any remaining CppBedrock processes
    exec('pkill -f CppBedrock', (err) => {
      if (err) {
        console.log('No CppBedrock process found to kill');
      }
    });
    return res.json({ success: true, message: 'No tracked process, sent pkill' });
  }

  try {
    serverProcess.kill('SIGTERM');

    // Force kill after 3 seconds if still alive
    setTimeout(() => {
      if (serverProcess) {
        serverProcess.kill('SIGKILL');
        serverProcess = null;
      }
    }, 3000);

    console.log('CppBedrock stopped');
    res.json({ success: true });
  } catch (err) {
    console.error('Failed to stop server:', err);
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// POST /api/run-test — run ./integration_test <scenario> in build/
// ──────────────────────────────────────────────────────────────
app.post('/api/run-test', (req, res) => {
  const { scenario } = req.body;
  if (!scenario) return res.status(400).json({ error: 'Missing scenario' });

  try {
    const testProcess = spawn('./integration_test', [String(scenario)], {
      cwd: BUILD_DIR,
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    testProcess.stdout.on('data', (data) => {
      serverOutput.push(data.toString());
      if (serverOutput.length > 500) serverOutput.shift();
    });

    testProcess.stderr.on('data', (data) => {
      serverOutput.push(data.toString());
      if (serverOutput.length > 500) serverOutput.shift();
    });

    testProcess.on('close', (code) => {
      serverOutput.push(`[integration_test scenario ${scenario} exited with code ${code}]\n`);
      console.log(`integration_test scenario ${scenario} exited with code ${code}`);
    });

    testProcess.on('error', (err) => {
      serverOutput.push(`[integration_test error: ${err.message}]\n`);
    });

    console.log(`Running integration_test scenario ${scenario}`);
    res.json({ success: true, scenario });
  } catch (err) {
    console.error('Failed to run test:', err);
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// GET /api/server-output — get buffered stdout/stderr
// ──────────────────────────────────────────────────────────────
app.get('/api/server-output', (req, res) => {
  const output = serverOutput.join('');
  serverOutput = []; // Clear after reading
  res.json({ output });
});

// ──────────────────────────────────────────────────────────────
// GET /api/server-status — check if CppBedrock is running
// ──────────────────────────────────────────────────────────────
app.get('/api/server-status', (req, res) => {
  res.json({ running: serverProcess !== null });
});

// ──────────────────────────────────────────────────────────────
// GET /api/node-ops — read per-node operation logs
// ──────────────────────────────────────────────────────────────
app.get('/api/node-ops', (req, res) => {
  const max = Number(req.query.max) || 500;
  const logsDir = path.join(BUILD_DIR, 'logs');

  try {
    if (!fs.existsSync(logsDir)) {
      return res.status(404).json({ error: 'No logs directory' });
    }

    const files = fs.readdirSync(logsDir).filter((f) => f.match(/^node_\d+_ops\.csv$/));
    const nodeData = {};

    files.forEach((file) => {
      const nodeId = file.match(/node_(\d+)_ops/)[1];
      const content = fs.readFileSync(path.join(logsDir, file), 'utf8');
      const lines = content.trim().split('\n').slice(0, max);
      nodeData[nodeId] = lines.map((line) => {
        const [sequence, ts] = line.split(',');
        return { sequence: Number(sequence), ts: Number(ts) };
      }).filter((p) => !isNaN(p.sequence) && !isNaN(p.ts));
    });

    res.json(nodeData);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ──────────────────────────────────────────────────────────────
// GET /api/cluster-config — full cluster state
// ──────────────────────────────────────────────────────────────
app.get('/api/cluster-config', (req, res) => {
  try {
    const summary = getClusterSummary();
    res.json(summary);
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// POST /api/cluster/add-node — add a node to a cluster
app.post('/api/cluster/add-node', (req, res) => {
  try {
    const { cluster, role = 'Replica', byzantine = false } = req.body;
    if (cluster === undefined || !CLUSTERS[cluster]) {
      return res.status(400).json({ error: `Invalid cluster: ${cluster}` });
    }

    const config = loadConfig();
    const entities = config.entities;
    const clusterMap = buildClusterMap(entities);

    const newId = Math.max(...entities.map((e) => e.id)) + 1;
    const newEntity = { id: newId, role, byzantine, cluster: Number(cluster), peers: [] };

    // Insert after last node in cluster
    const clusterNodes = clusterMap[cluster] || [];
    if (clusterNodes.length > 0) {
      const lastNode = Math.max(...clusterNodes);
      const idx = entities.findIndex((e) => e.id === lastNode);
      entities.splice(idx + 1, 0, newEntity);
    } else {
      entities.push(newEntity);
    }

    regenerateConfig(config);
    saveConfig(config);

    res.json({
      success: true,
      nodeId: newId,
      cluster,
      totalNodes: entities.length,
      f: Math.floor((entities.length - 1) / 3),
    });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// POST /api/cluster/remove-node — remove a node
app.post('/api/cluster/remove-node', (req, res) => {
  try {
    const { nodeId } = req.body;
    const config = loadConfig();
    const entities = config.entities;
    const idx = entities.findIndex((e) => e.id === nodeId);

    if (idx === -1) return res.status(404).json({ error: `Node ${nodeId} not found` });

    const removed = entities.splice(idx, 1)[0];
    regenerateConfig(config);
    saveConfig(config);

    res.json({
      success: true,
      removedNode: removed,
      totalNodes: entities.length,
      f: Math.floor((entities.length - 1) / 3),
    });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// POST /api/cluster/set-property — update node properties
app.post('/api/cluster/set-property', (req, res) => {
  try {
    const { nodeId, byzantine, role } = req.body;
    const config = loadConfig();
    const entity = config.entities.find((e) => e.id === nodeId);

    if (!entity) return res.status(404).json({ error: `Node ${nodeId} not found` });

    const changed = [];
    if (byzantine !== undefined) {
      entity.byzantine = !!byzantine;
      changed.push(`byzantine=${entity.byzantine}`);
    }
    if (role !== undefined) {
      entity.role = role;
      changed.push(`role=${role}`);
    }

    saveConfig(config);
    res.json({ success: true, nodeId, changed });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// POST /api/cluster/move-node — move a node to a different cluster
app.post('/api/cluster/move-node', (req, res) => {
  try {
    const { nodeId, targetCluster } = req.body;
    if (!CLUSTERS[targetCluster]) {
      return res.status(400).json({ error: `Invalid cluster: ${targetCluster}` });
    }

    const config = loadConfig();
    const entity = config.entities.find((e) => e.id === nodeId);
    if (!entity) return res.status(404).json({ error: `Node ${nodeId} not found` });

    entity.cluster = Number(targetCluster);
    regenerateConfig(config);
    saveConfig(config);

    res.json({ success: true, nodeId, newCluster: targetCluster });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

// ──────────────────────────────────────────────────────────────
// Start server
// ──────────────────────────────────────────────────────────────
app.listen(PORT, () => {
  console.log(`CppBedrock web UI running at http://localhost:${PORT}`);
});

// ─── Helpers ───

function loadConfig() {
  return yaml.load(fs.readFileSync(CONFIG_PATH, 'utf8'));
}

function saveConfig(config) {
  fs.writeFileSync(CONFIG_PATH, yaml.dump(config, { flowLevel: 3, noRefs: true }));
}

function buildClusterMap(entities) {
  const map = {};
  for (let i = 0; i < 10; i++) map[i] = [];

  entities.forEach((ent) => {
    if (ent.cluster !== undefined && ent.cluster !== null) {
      map[ent.cluster].push(ent.id);
    } else {
      // Fallback: infer from original layout
      const nid = ent.id;
      let cid;
      if (nid <= 4) cid = 0;
      else if (nid <= 7) cid = 1;
      else if (nid <= 10) cid = 2;
      else if (nid <= 13) cid = 3;
      else if (nid <= 16) cid = 4;
      else if (nid <= 19) cid = 5;
      else if (nid <= 22) cid = 6;
      else if (nid <= 25) cid = 7;
      else if (nid <= 28) cid = 8;
      else cid = 9;
      ent.cluster = cid;
      map[cid].push(nid);
    }
  });
  return map;
}

function regenerateConfig(config) {
  const entities = config.entities;
  const clusterMap = buildClusterMap(entities);
  const allIds = entities.map((e) => e.id).sort((a, b) => a - b);

  // Update peers
  entities.forEach((ent) => { ent.peers = [...allIds]; });

  // Rebuild node-to-cluster
  const nodeToCluster = {};
  for (const [cid, nodes] of Object.entries(clusterMap)) {
    nodes.forEach((nid) => { nodeToCluster[nid] = Number(cid); });
  }

  // Rebuild delays
  const delays = [];
  for (const src of allIds) {
    for (const dst of allIds) {
      if (src === dst) continue;
      const delay = LATENCY_MATRIX[nodeToCluster[src]][nodeToCluster[dst]];
      delays.push({ from: src, to: dst, delay });
    }
  }
  config.delays = delays;

  return config;
}

function getClusterSummary() {
  const config = loadConfig();
  const entities = config.entities;
  const clusterMap = buildClusterMap(entities);
  const n = entities.length;
  const f = Math.floor((n - 1) / 3);

  const clusters = {};
  for (const [cid, nodeIds] of Object.entries(clusterMap)) {
    const nodes = nodeIds.map((nid) => {
      const ent = entities.find((e) => e.id === nid);
      return {
        id: ent.id,
        role: ent.role,
        byzantine: ent.byzantine || false,
        cluster: Number(cid),
      };
    });
    clusters[cid] = {
      ...CLUSTERS[cid],
      nodes,
    };
  }

  return { clusters, n, f, quorum: 2 * f + 1, entities };
}