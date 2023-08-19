#ifndef CAM_STUFF_
#define CAM_STUFF_

String index_html =   "<html>\n \
<head>\n \
<title> WebSockets Client</title>\n \
<script src='http://code.jquery.com/jquery-1.9.1.min.js'></script>\n \
AI THINKER</head>\n \
<body>\n \
<br>\n \
<img id='live' src=''>\n \
</body>\n \
</html>\n \
<script>\n \
jQuery(function($){\n \
if (!('WebSocket' in window)) {\n \
alert('Your browser does not support web sockets');\n \
}else{\n \
setup();\n \
}\n \
function setup(){\n \
var host = 'ws://server_ip:81';\n \
var socket = new WebSocket(host);\n \
socket.binaryType = 'arraybuffer';\n \
if(socket){\n \
socket.onopen = function(){\n \
}\n \
socket.onmessage = function(msg){\n \
var bytes = new Uint8Array(msg.data);\n \
var binary= '';\n \
var len = bytes.byteLength;\n \
for (var i = 0; i < len; i++) {\n \
binary += String.fromCharCode(bytes[i])\n \
}\n \
var img = document.getElementById('live');\n \
img.src = 'data:image/jpg;base64,'+window.btoa(binary);\n \
}\n \
socket.onclose = function(){\n \
showServerResponse('The connection has been closed.');\n \
}\n \
}\n \
}\n \
});\n \
</script>";


#endif