import {
  ESPLoader,
  Transport,
} from "https://cdn.jsdelivr.net/npm/esptool-js@0.5.4/bundle.min.js";

const dom_connect_btn = document.getElementById("connectDevice");
const dom_flash_log = document.querySelector(".flash-log");
const dom_download_btn = document.querySelector(".download-firmware");
const dom_flash_btn = document.querySelector(".flash-firmware");

let espLoader = null;

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

const downloadClick = async (e) => {
  log("正在从 GitHub 获取固件地址...");
  const projectURL =
    "https://api.github.com/repos/MacheteHot/c3Homekit/releases/latest";
  const response = await fetch(projectURL);
  const data = await response.json();
  const firmwareURL = data.assets[0].browser_download_url;
  log("下载后请点击上传按钮进行刷写");
  // 此处直接下载文件
  window.open(firmwareURL, "_blank");
};

const onButtonClick = async (e) => {
  dom_flash_btn.disabled = true;
  const port = await navigator.serial.requestPort({ filters });

  const transport = new Transport(port);

  espLoader = new ESPLoader({
    transport,
    terminal,
    baudrate: 115200,
    terminal,
  });

  try {
    terminal.clean();
    await espLoader.main(); //自动握手、检测、复位
    terminal.writeLine("连接成功");
    dom_flash_btn.disabled = false;
  } catch (error) {
    log("❌ 错误: " + error.message);
    dom_flash_btn.disabled = true;
    return;
  }
};

const onFlashClick = async (e) => {
  if (!espLoader) {
    log("请先连接设备");
    return;
  }
  dom_flash_btn.disabled = true;
  const input = document.createElement("input");
  input.type = "file";
  input.accept = ".bin";
  input.addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) {
      log("没有选择文件");
      return;
    }
    const reader = new FileReader();
    reader.onload = async (e) => {
      const { result } = e.target;
      // ArrayBuffer -> binary string
      const uint8 = new Uint8Array(result);
      let binStr = "";
      for (let i = 0; i < uint8.length; i++) {
        binStr += String.fromCharCode(uint8[i]);
      }
      try {
        log("开始刷写...");
        await espLoader.writeFlash({
          fileArray: [{ data: binStr, address: 0 }],
          flashSize: "4MB", // 强制4MB
          eraseAll: true, // 擦除所有
          compress: true,
          onProgress: (written, total) => {
            const percent = Math.floor((written / total) * 100);
            log(`刷写进度: ${percent}%`);
          },
        });
        log("刷写成功");
        await espLoader.flashFinish(true);
        log("设备刷写完成正在重启... 请到右侧配网工具进行配网");
    
      } catch (error) {
        console.error(error);
        log("刷写失败: " + error.message);
      } finally {
        dom_flash_btn.disabled = false;
      }
    };
    reader.readAsArrayBuffer(file);
  });
  input.click();
  input.remove();
};

dom_connect_btn.addEventListener("click", onButtonClick);
dom_download_btn.addEventListener("click", downloadClick);
dom_flash_btn.addEventListener("click", onFlashClick);
