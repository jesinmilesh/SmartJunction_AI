const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 8081;
// You can place any traffic image as 'test_frame.jpg' in the edge_ai_server folder
const IMAGE_PATH = path.join(__dirname, 'test_frame.jpg');

const server = http.createServer((req, res) => {
  console.log(`[MockCam] Request: ${req.url}`);
  
  if (req.url === '/capture' || req.url === '/stream') {
    if (fs.existsSync(IMAGE_PATH)) {
      res.writeHead(200, { 'Content-Type': 'image/jpeg' });
      fs.createReadStream(IMAGE_PATH).pipe(res);
    } else {
      // Return a basic error if image is missing
      res.writeHead(404);
      res.end('No test image found. Please place test_frame.jpg in the server folder.');
    }
  } else {
    res.writeHead(404);
    res.end('Not found');
  }
});

server.listen(PORT, () => {
  console.log(`[MockCam] Mock Camera running at http://localhost:${PORT}`);
  console.log(`[MockCam] Serving: ${IMAGE_PATH}`);
});
