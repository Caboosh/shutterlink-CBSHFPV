// ============================================================================
// site.js — shared chrome behaviour for the ShutterLink docs site
// Theme toggle, mobile nav, active-link highlighting, TOC scroll-spy.
// No dependencies, no build step — same philosophy as the firmware's own
// embedded Web UI.
// ============================================================================
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const THEME_KEY = "shutterlink.theme";

  /* ---------- theme toggle ---------- */
  function applyThemeIcon(theme) {
    const ico = $("themeIco");
    if (ico) ico.setAttribute("href", theme === "dark" ? "#i-sun" : "#i-moon");
  }
  function initTheme() {
    const saved = (() => { try { return localStorage.getItem(THEME_KEY); } catch (_) { return null; } })();
    const theme = saved || document.documentElement.getAttribute("data-theme") || "dark";
    document.documentElement.setAttribute("data-theme", theme);
    applyThemeIcon(theme);
    const btn = $("themeBtn");
    if (btn) {
      btn.addEventListener("click", () => {
        const cur = document.documentElement.getAttribute("data-theme");
        const next = cur === "dark" ? "light" : "dark";
        document.documentElement.setAttribute("data-theme", next);
        applyThemeIcon(next);
        try { localStorage.setItem(THEME_KEY, next); } catch (_) {}
      });
    }
  }

  /* ---------- mobile nav ---------- */
  function initNav() {
    const toggle = $("navToggle");
    const nav = document.querySelector("nav.site-nav");
    if (toggle && nav) {
      toggle.addEventListener("click", () => nav.classList.toggle("open"));
      nav.querySelectorAll("a").forEach((a) =>
        a.addEventListener("click", () => nav.classList.remove("open"))
      );
    }
    // Mark the current page's nav link active.
    const here = location.pathname.replace(/\/index\.html$/, "/").split("/").pop() || "index.html";
    document.querySelectorAll("nav.site-nav a[href]").forEach((a) => {
      const target = a.getAttribute("href").split("/").pop();
      if (target === here || (here === "" && target === "index.html")) {
        a.classList.add("active");
      }
    });
  }

  /* ---------- TOC scroll-spy ---------- */
  function initToc() {
    const toc = document.querySelector(".toc");
    if (!toc) return;
    const links = Array.from(toc.querySelectorAll("a[href^='#']"));
    if (!links.length) return;
    const targets = links
      .map((a) => document.getElementById(a.getAttribute("href").slice(1)))
      .filter(Boolean);
    if (!targets.length) return;

    const setActive = (id) => {
      links.forEach((a) => a.classList.toggle("active", a.getAttribute("href") === "#" + id));
    };

    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting) setActive(e.target.id);
        });
      },
      { rootMargin: "-90px 0px -70% 0px", threshold: 0 }
    );
    targets.forEach((t) => io.observe(t));
  }

  document.addEventListener("DOMContentLoaded", () => {
    initTheme();
    initNav();
    initToc();
  });
})();
