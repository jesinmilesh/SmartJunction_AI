/**
 * download-model.js
 * -----------------
 * Downloads the YOLOv8n ONNX model (no Python required).
 * Handles up to 5 HTTP redirects automatically.
 *
 * Usage:
 *   node download-model.js
 */
'use strict';

const https = require('https');
const http  = require('http');
const fs    = require('fs');
const path  = require('path');

const MODEL_URL  = 'https://raw.githubusercontent.com/yoobright/yolo-onnx/master/yolov8n.onnx';
const MODEL_PATH = path.join(__dirname, 'yolov8n.onnx');
const MAX_REDIRECTS = 5;

// ── Already downloaded? ───────────────────────────────────────
if (fs.existsSync(MODEL_PATH)) {
  const sizeMB = (fs.statSync(MODEL_PATH).size / 1e6).toFixed(1);
  console.log(`[OK] yolov8n.onnx already exists (${sizeMB} MB)`);
  console.log(`[OK] Path: ${MODEL_PATH}`);
  process.exit(0);
}

console.log('[Download] YOLOv8n ONNX model (~12 MB)');
console.log(`[Download] Source: ${MODEL_URL}`);
console.log('[Download] This may take a minute on slow connections…\n');

/**
 * Download a URL to destPath, following up to maxRedirects.
 */
function download(url, destPath, redirectsLeft) {
  return new Promise((resolve, reject) => {
    if (redirectsLeft < 0) {
      return reject(new Error('Too many redirects'));
    }

    const lib = url.startsWith('https') ? https : http;

    lib.get(url, (res) => {
      const { statusCode } = res;

      // ── Redirect ─────────────────────────────────────────
      if (statusCode === 301 || statusCode === 302 || statusCode === 307 || statusCode === 308) {
        const location = res.headers.location;
        if (!location) return reject(new Error('Redirect with no Location header'));
        res.resume(); // consume and discard response body
        console.log(`[Download] Redirect (${statusCode}) → ${location}`);
        resolve(download(location, destPath, redirectsLeft - 1));
        return;
      }

      // ── Error status ──────────────────────────────────────
      if (statusCode !== 200) {
        res.resume();
        return reject(new Error(`HTTP ${statusCode} from ${url}`));
      }

      // ── Streaming download ────────────────────────────────
      const total    = parseInt(res.headers['content-length'] || '0', 10);
      let received   = 0;
      const dest     = fs.createWriteStream(destPath);

      res.on('data', (chunk) => {
        received += chunk.length;
        if (total > 0) {
          const pct = ((received / total) * 100).toFixed(1);
          process.stdout.write(
            `\r[Download] ${(received / 1e6).toFixed(1)} / ${(total / 1e6).toFixed(1)} MB  (${pct}%)   `,
          );
        } else {
          process.stdout.write(`\r[Download] ${(received / 1e6).toFixed(1)} MB received…`);
        }
      });

      res.pipe(dest);

      dest.on('finish', () => {
        dest.close(() => {
          console.log('\n[Download] ✓ yolov8n.onnx saved!');
          console.log(`[Download] Path: ${destPath}`);
          console.log('[Next]     Run: node server.js');
          resolve();
        });
      });

      dest.on('error', (err) => {
        fs.unlink(destPath, () => {}); // clean up partial file
        reject(err);
      });

      res.on('error', (err) => {
        dest.destroy();
        fs.unlink(destPath, () => {});
        reject(err);
      });
    }).on('error', (err) => {
      reject(err);
    });
  });
}

// ── Run ───────────────────────────────────────────────────────
download(MODEL_URL, MODEL_PATH, MAX_REDIRECTS)
  .then(() => process.exit(0))
  .catch((err) => {
    console.error('\n[Error]', err.message);
    // Remove partial file if it exists
    if (fs.existsSync(MODEL_PATH)) fs.unlinkSync(MODEL_PATH);
    process.exit(1);
  });
