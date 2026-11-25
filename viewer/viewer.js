import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { RoomEnvironment } from "three/addons/environments/RoomEnvironment.js";

const byId = (id) => document.getElementById(id);
const canvas = byId("canvas");
const status = byId("status");

function disposeModel(root) {
  const resources = new Set();
  root.traverse((object) => {
    if (object.geometry) resources.add(object.geometry);
    for (const material of [].concat(object.material || [])) {
      resources.add(material);
      for (const value of Object.values(material)) {
        if (value?.isTexture) resources.add(value);
      }
    }
  });
  for (const resource of resources) {
    if (resource.isTexture) resource.source?.data?.close?.();
    resource.dispose();
  }
}

function createStage() {
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  renderer.toneMapping = THREE.NeutralToneMapping;
  renderer.toneMappingExposure = 0.85;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.autoUpdate = false;
  renderer.shadowMap.type = THREE.PCFShadowMap;
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0xf1f3ed);
  scene.fog = new THREE.Fog(0xf1f3ed, 6, 14);
  const camera = new THREE.PerspectiveCamera(34, 1, 0.015, 70);
  const controls = new OrbitControls(camera, canvas);
  controls.enablePan = false;
  controls.enabled = false;
  controls.minDistance = 0.22;
  controls.maxDistance = 14;
  const direction = new THREE.Vector3(0.32, 0.18, 1).normalize();

  const studio = new RoomEnvironment();
  const generator = new THREE.PMREMGenerator(renderer);
  scene.environment = generator.fromScene(studio, 0.04).texture;
  scene.environmentIntensity = 0.28;
  studio.dispose();
  generator.dispose();
  scene.add(new THREE.HemisphereLight(0xffffff, 0xd6d3bf, 0.5));
  const sun = new THREE.DirectionalLight(0xfff8ef, 1.5);
  sun.position.set(-3, 5, 4);
  sun.castShadow = true;
  sun.shadow.mapSize.set(1024, 1024);
  Object.assign(sun.shadow.camera, { left: -2, right: 2, top: 2, bottom: -2, near: 0.5, far: 14 });
  sun.shadow.camera.updateProjectionMatrix();
  sun.shadow.normalBias = 0.005;
  scene.add(sun);

  const floor = new THREE.Group();
  const shadow = new THREE.Mesh(new THREE.PlaneGeometry(18, 18),
    new THREE.ShadowMaterial({ opacity: 0.14, depthWrite: false }));
  shadow.rotation.x = -Math.PI / 2;
  shadow.receiveShadow = true;
  const grid = new THREE.GridHelper(18, 72, 0xb2beaf, 0xb2beaf);
  grid.material.transparent = true;
  grid.material.opacity = 0.24;
  grid.material.depthWrite = false;
  grid.position.y = -0.002;
  floor.add(grid, shadow);
  scene.add(floor);
  let model;
  let bounds;
  let frame = 0;

  function redraw() {
    if (frame) return;
    frame = requestAnimationFrame(() => {
      frame = 0;
      floor.visible = !!model && camera.position.y > floor.position.y;
      renderer.render(scene, camera);
    });
  }

  function fit() {
    if (!model) return;
    const right = new THREE.Vector3().crossVectors(camera.up, direction).normalize();
    const up = new THREE.Vector3().crossVectors(direction, right);
    const tanY = Math.tan(THREE.MathUtils.degToRad(camera.fov / 2));
    const tanX = tanY * camera.aspect;
    let distance = 0;
    for (const x of [bounds.min.x, bounds.max.x]) {
      for (const y of [bounds.min.y, bounds.max.y]) {
        for (const z of [bounds.min.z, bounds.max.z]) {
          const corner = new THREE.Vector3(x, y, z);
          distance = Math.max(distance, corner.dot(direction) +
            Math.max(Math.abs(corner.dot(right)) / tanX, Math.abs(corner.dot(up)) / tanY));
        }
      }
    }
    controls.target.set(0, 0, 0);
    camera.position.copy(direction).multiplyScalar(distance * 1.16);
    controls.update();
    redraw();
  }

  function resize() {
    const width = Math.max(1, canvas.clientWidth);
    const height = Math.max(1, canvas.clientHeight);
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    redraw();
  }
  resize();
  new ResizeObserver(resize).observe(byId("stage"));
  controls.addEventListener("change", redraw);
  byId("in").onclick = () => controls.dollyIn(0.8);
  byId("out").onclick = () => controls.dollyOut(0.8);
  byId("fit").onclick = fit;
  canvas.addEventListener("keydown", (event) => {
    if (!model) return;
    switch (event.key) {
      case "ArrowLeft": controls.rotateLeft(0.12); break;
      case "ArrowRight": controls.rotateLeft(-0.12); break;
      case "ArrowUp": controls.rotateUp(0.12); break;
      case "ArrowDown": controls.rotateUp(-0.12); break;
      case "+": case "=": controls.dollyIn(0.8); break;
      case "-": controls.dollyOut(0.8); break;
      case "Home": fit(); break;
      default: return;
    }
    event.preventDefault();
  });

  return function display(root) {
    const box = new THREE.Box3().setFromObject(root, true);
    const size = box.getSize(new THREE.Vector3());
    const span = Math.max(size.x, size.y, size.z);
    if (!Number.isFinite(span) || span <= 0) throw new Error("표시할 수 있는 메시가 없습니다.");
    const group = new THREE.Group();
    group.add(root);
    group.scale.setScalar(2 / span);
    group.position.copy(box.getCenter(new THREE.Vector3())).multiplyScalar(-2 / span);
    let triangles = 0;
    root.traverse((item) => {
      if (!item.isMesh) return;
      item.castShadow = true;
      const geometry = item.geometry;
      const count = geometry.index?.count ?? geometry.attributes.position.count;
      triangles += Math.floor(count / 3) * (item.isInstancedMesh ? item.count : 1);
    });
    if (!triangles) throw new Error("삼각형 메시가 없습니다.");
    if (model) {
      scene.remove(model);
      disposeModel(model);
    }
    model = group;
    scene.add(model);
    bounds = new THREE.Box3().setFromObject(model, true);
    floor.position.y = bounds.min.y - 0.008;
    renderer.shadowMap.needsUpdate = true;
    controls.enabled = true;
    for (const id of ["in", "out", "fit"]) byId(id).disabled = false;
    fit();
    return triangles;
  };
}

function startViewer() {
  const display = createStage();
  const loader = new GLTFLoader();
  let sequence = 0;
  async function open(read) {
    const ticket = ++sequence;
    status.textContent = "모델을 불러오는 중…";
    let root;
    try {
      const { name, bytes } = await read();
      if (ticket !== sequence) return;
      if (bytes.byteLength < 12 || new DataView(bytes).getUint32(0, true) !== 0x46546c67) {
        throw new Error("GLB 파일 형식을 확인하세요.");
      }
      root = (await loader.parseAsync(bytes, "")).scene;
      if (ticket !== sequence) return;
      const triangles = display(root);
      root = null; // The stage now owns the model.
      byId("name").textContent = byId("name").title = name;
      byId("stats").textContent = "삼각형 " + triangles.toLocaleString("ko-KR") + "개  ·  " +
        (bytes.byteLength / 1024).toLocaleString("ko-KR", { maximumFractionDigits: 0 }) + " KB";
      document.title = name + " · Fauna Studio";
      status.textContent = "";
    } catch (error) {
      if (ticket === sequence) status.textContent = "모델을 열 수 없습니다. " + error.message;
    } finally {
      if (root) disposeModel(root);
    }
  }

  byId("open").onclick = () => byId("file").click();
  byId("file").onchange = (event) => {
    const file = event.target.files[0];
    if (file) open(async () => ({ name: file.name, bytes: await file.arrayBuffer() }));
    event.target.value = "";
  };
  const token = new URLSearchParams(location.search).get("model");
  if (!token) {
    status.textContent = "열어 볼 GLB 파일을 선택하세요.";
    return;
  }
  open(async () => {
    const response = await fetch("/models/" + encodeURIComponent(token));
    if (!response.ok) throw new Error("앱에서 파일을 다시 선택하세요.");
    return { name: decodeURIComponent(response.headers.get("X-Model-Name")),
      bytes: await response.arrayBuffer() };
  });
}

try {
  startViewer();
} catch (error) {
  console.error(error);
  byId("open").disabled = true;
  status.textContent = "3D 화면을 시작할 수 없습니다. WebGL 2를 지원하는 브라우저에서 그래픽 가속을 확인하세요.";
}
