// Beispiel-Client für /rawws - im Browser oder Node (mit ws-Paket) nutzbar.
// Zeigt, wie man das Binärformat parst: [0xA5][ver][numLedsLE16][R,G,B,W]*n

const socket = new WebSocket("ws://WLED-IP/rawws");
socket.binaryType = "arraybuffer";

socket.onopen = () => console.log("rawws verbunden");

socket.onmessage = (evt) => {
  const buf = new Uint8Array(evt.data);
  if (buf[0] !== 0xA5) return; // falsches Paket / Magic-Byte prüfen
  const version = buf[1];
  const numLeds = buf[2] | (buf[3] << 8);

  const pixels = new Array(numLeds);
  let idx = 4;
  for (let i = 0; i < numLeds; i++) {
    pixels[i] = {
      r: buf[idx++],
      g: buf[idx++],
      b: buf[idx++],
      w: buf[idx++],
    };
  }

  // pixels[i] enthält jetzt die echten, unvermischten RGBW-Werte
  // von Pixel i - über alle Segmente hinweg, exakt wie im Framebuffer.
  // Hier z.B. weiterverarbeiten, an Canvas rendern, an eine andere App
  // weiterschicken usw.
};

socket.onclose = () => console.log("rawws getrennt");
