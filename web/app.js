function sidebar(active) {
  var el = document.getElementById("sidebar");
  if (!el) return;
  el.innerHTML =
    '<div class="sidebar-brand">' +
      '<div class="sidebar-kicker">RFC-1166</div>' +
      '<div class="sidebar-title">Kerry Cam</div>' +
    '</div>' +
    '<nav class="sidebar-nav">' +
      '<a class="sidebar-a' + (active === "index" ? " active" : "") + '" href="/index.html">Overview</a>' +
      '<a class="sidebar-a' + (active === "image" ? " active" : "") + '" href="/image.html">Camera</a>' +
      '<a class="sidebar-a' + (active === "overlay" ? " active" : "") + '" href="/overlay.html">Overlay</a>' +
      '<a class="sidebar-a' + (active === "network" ? " active" : "") + '" href="/network.html">Network</a>' +
      '<a class="sidebar-a' + (active === "uploads" ? " active" : "") + '" href="/uploads.html">Uploads</a>' +
      '<a class="sidebar-a' + (active === "system" ? " active" : "") + '" href="/system.html">System</a>' +
    '</nav>' +
    '<div class="sidebar-foot">' +
      'Setup AP · 192.168.4.1<br>' +
      '<a href="#" onclick="logout(); return false;" style="color:var(--muted);font-size:12px">Logout</a>' +
    '</div>';
}

function ajax(option) {
  var init = Object.assign({
    method: "GET",
    url: "",
    data: {},
    success: function () {},
    error: function () {}
  }, option);
  init.method = init.method.toUpperCase();
  var xhr = new XMLHttpRequest();
  xhr.timeout = 60000;
  xhr.responseType = "json";
  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;
    if (xhr.status >= 200 && xhr.status < 300) init.success(xhr.response || {});
    else init.error(xhr.status, xhr.response || {});
  };
  if (init.method === "GET") {
    var params = Object.keys(init.data).map(function (k) {
      return encodeURIComponent(k) + "=" + encodeURIComponent(init.data[k]);
    }).join("&");
    xhr.open("GET", init.url + (params ? "?" + params : ""), true);
    xhr.send();
  } else {
    xhr.open("POST", init.url, true);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.send(JSON.stringify(init.data || {}));
  }
}

function toast(msg, kind) {
  var el = document.getElementById("toast");
  if (!el) {
    alert(msg);
    return;
  }
  el.textContent = msg;
  el.className = "notice " + (kind || "");
  el.style.display = "block";
}

function requireAuth(next) {
  ajax({
    url: "/api/auth",
    success: function (res) {
      if (res.ok) next();
      else location.href = "/login.html";
    },
    error: function () { location.href = "/login.html"; }
  });
}

function logout() {
  ajax({
    method: "POST",
    url: "/api/logout",
    success: function () { location.href = "/login.html"; },
    error: function () { location.href = "/login.html"; }
  });
}

function togglePwd(box, id) {
  var input = document.getElementById(id);
  if (input) input.type = box.checked ? "text" : "password";
}

function togglePortal(show) {
  var block = document.getElementById("portalBlock");
  if (block) block.classList.toggle("show", !!show);
}
