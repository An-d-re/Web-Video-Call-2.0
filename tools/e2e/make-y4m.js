// 生成 y4m 假摄像头视频文件，给 Chromium 的 --use-file-for-fake-video-capture 用。
//
// 为什么不直接用 --use-fake-device-for-media-stream 那个内置图案设备：
// 在 headless 模式下它会间歇性不产帧——同一份代码、同样的步骤，有时一端
// outbound video 一个字节都不发（getStats 里 tx=0），e2e 就一会儿绿一会儿红，
// 而且红的时候根本分不清是页面代码的问题还是浏览器的脾气。
// 换成文件源之后帧是确定性的，测试结果才只反映被测代码。
//
// 用法：node tools/e2e/make-y4m.js [输出路径]
// 作为模块：require('./make-y4m')(outPath)
const fs = require('fs');
const path = require('path');

const W = 320, H = 240, FPS = 30, FRAMES = 30;

function makeY4m(outFile) {
  const ySize = W * H, cSize = (W / 2) * (H / 2);
  const out = outFile || path.join(__dirname, 'fake-camera.y4m');
  fs.mkdirSync(path.dirname(out), { recursive: true });

  const fd = fs.openSync(out, 'w');
  fs.writeSync(fd, `YUV4MPEG2 W${W} H${H} F${FPS}:1 Ip A1:1 C420jpeg\n`);
  for (let f = 0; f < FRAMES; f++) {
    const y = Buffer.alloc(ySize);
    const u = Buffer.alloc(cSize);
    const v = Buffer.alloc(cSize);
    // 一个来回移动的方块 + 每帧不同的整体亮度：肉眼能看出"在动"
    const boxX = Math.floor((W - 80) * (0.5 + 0.5 * Math.sin((f / FRAMES) * Math.PI * 2)));
    const luma = 60 + Math.floor(120 * (0.5 + 0.5 * Math.sin((f / FRAMES) * Math.PI * 2)));
    for (let py = 0; py < H; py++) {
      for (let px = 0; px < W; px++) {
        const inBox = px >= boxX && px < boxX + 80 && py >= 80 && py < 160;
        y[py * W + px] = inBox ? 235 : luma;
      }
    }
    u.fill(128); v.fill(128);
    fs.writeSync(fd, 'FRAME\n');
    fs.writeSync(fd, y); fs.writeSync(fd, u); fs.writeSync(fd, v);
  }
  fs.closeSync(fd);
  return out;
}

module.exports = makeY4m;

if (require.main === module) {
  const out = makeY4m(process.argv[2]);
  console.log(`已生成 ${out}（${(fs.statSync(out).size / 1048576).toFixed(1)} MB）`);
}
