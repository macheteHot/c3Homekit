import {
  ESPLoader,
  Transport,
  getStubJsonByChipName,
} from "https://unpkg.com/esptool-js@0.5.4/bundle.js";

const dom_flash_btn = document.getElementById("flashDevice");
const dom_flash_log = document.querySelector(".flash-log");

function log(msg) {
  if (!msg) return;
  if (typeof msg === "string") {
    const msgDiv = document.createElement("div");
    msgDiv.textContent = msg;
    dom_flash_log.appendChild(msgDiv);
  }
  if (msg instanceof HTMLElement) {
    dom_flash_log.appendChild(msg);
  }
  dom_flash_log.scrollTop = dom_flash_log.scrollHeight;
}

const filters = [
  { usbVendorId: 4292, usbProductId: 60000 },
  { usbVendorId: 1027, usbProductId: 24592 },
  { usbVendorId: 12346, usbProductId: 4097 },
  { usbVendorId: 12346, usbProductId: 4098 },
  { usbVendorId: 12346, usbProductId: 2 },
  { usbVendorId: 12346, usbProductId: 9 },
  { usbVendorId: 6790, usbProductId: 21972 },
  { usbVendorId: 6790, usbProductId: 29987 },
  { usbVendorId: 1027, usbProductId: 24577 },
];
const terminal = {
  clean: () => {
    dom_flash_log.innerHTML = "";
  },
  write: (msg) => {
    const lastChild = dom_flash_log.lastChild;
    if (!lastChild) {
      const msgDiv = document.createElement("div");
      msgDiv.textContent = msg;
      dom_flash_log.appendChild(msgDiv);
    } else {
      dom_flash_log.lastChild.textContent += msg;
    }
  },
  writeLine: log,
};

// 下载固件并显示进度条
async function downloadFirmwareWithProgress(url, onProgress) {
  const response = await fetch(url);
  if (!response.ok) throw new Error("固件下载失败");
  const contentLength = response.headers.get("content-length");
  if (!contentLength) {
    // 无法获取长度时直接返回 ArrayBuffer
    return await response.arrayBuffer();
  }
  const total = parseInt(contentLength, 10);
  const reader = response.body.getReader();
  let received = 0;
  let chunks = [];
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    if (onProgress) onProgress(received, total);
  }
  // 合并所有 chunk
  let firmware = new Uint8Array(received);
  let pos = 0;
  for (let chunk of chunks) {
    firmware.set(chunk, pos);
    pos += chunk.length;
  }
  return firmware.buffer;
}

const onButtonClick = async (e) => {
  dom_flash_btn.disabled = true;
  const port = await navigator.serial.requestPort({ filters });

  const transport = new Transport(port);

  const loader = new ESPLoader({
    transport,
    terminal,
    baudrate: 115200,
    terminal,
  });

  try {
    terminal.clean();
    const result = await loader.main(); //自动握手、检测、复位
    terminal.writeLine("连接成功");
  } catch (error) {
    log("❌ 错误: " + error.message);
    dom_flash_btn.disabled = false;
    return;
  }

  log("正在从 GitHub 获取固件地址...");
  const projectURL =
    "https://api.github.com/repos/MacheteHot/c3Homekit/releases/latest";
  const response = await fetch(projectURL);
  const data = await response.json();
  const firmwareURL = data.assets[0].browser_download_url;
  log("固件地址获取成功");
  log("正在下载固件...");

  // 下载固件并显示进度
  let lastPercent = -1;
  const firmware = await downloadFirmwareWithProgress(
    firmwareURL,
    (loaded, total) => {
      const percent = Math.floor((loaded / total) * 100);
      if (percent !== lastPercent) {
        log(`下载进度: ${percent}%`);
        lastPercent = percent;
      }
    }
  );

  // // 3. 写入固件（实际协议需参考你的设备，以下为简单bulk写入示例）
  // const chunkSize = 4096;
  // for (let offset = 0; offset < firmware.byteLength; offset += chunkSize) {
  //   const chunk = new Uint8Array(
  //     firmware,
  //     offset,
  //     Math.min(chunkSize, firmware.byteLength - offset)
  //   );
  //   await device.transferOut(2, chunk); // 2为endpoint号，实际需查设备描述符
  // }

  // await device.close();
  // alert("刷写完成！");
};

dom_flash_btn.addEventListener("click", onButtonClick);
