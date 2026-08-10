(() => {
  const stage = document.getElementById("stage");
  const next = document.getElementById("next");
  const filter = document.getElementById("filter");
  const status = document.getElementById("status");
  const error = document.getElementById("error");
  const tagList = document.getElementById("tag-list");
  const tagSidebarState = document.getElementById("tag-sidebar-state");
  const addSourceOpen = document.getElementById("add-source-open");
  const sourceDialog = document.getElementById("source-dialog");
  const sourceInput = document.getElementById("source-input");
  const sourceFeedback = document.getElementById("source-feedback");
  const sourceForm = document.getElementById("source-dialog-body");
  const sourceClose = document.getElementById("source-close");
  const sourceAdd = document.getElementById("source-add");
  const mobileQuery = window.matchMedia("(max-width: 700px)");
  let loading = false;
  let sizeMode = "fit";
  let showHint = true;
  let resizeFrame = 0;
  let tagCatalogLoaded = false;
  let tagCatalogLoading = false;

  const RANDOM_ATTEMPTS = 3;
  const RANDOM_TIMEOUT_MS = 12_000;

  function wait(milliseconds) {
    return new Promise((resolve) => window.setTimeout(resolve, milliseconds));
  }

  function requestProblem(message, retryable) {
    const problem = new Error(message);
    problem.retryable = retryable;
    return problem;
  }

  function isTransientStatus(statusCode) {
    return [408, 425, 429].includes(statusCode) || statusCode >= 500;
  }

  function selectedTags() {
    return Array.from(tagList.querySelectorAll('input[type="checkbox"]:checked'))
      .map((input) => input.value);
  }

  async function fetchRandomOnce(extension, tags) {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), RANDOM_TIMEOUT_MS);

    try {
      const parameters = new URLSearchParams({ ext: extension });
      for (const tag of tags) parameters.append("tag", tag);
      const response = await fetch(`/api/random?${parameters.toString()}`, {
        cache: "no-store",
        headers: { accept: "application/json" },
        signal: controller.signal,
      });
      const contentType = response.headers.get("content-type") || "";
      const body = await response.text();

      if (!contentType.toLowerCase().includes("application/json")) {
        throw requestProblem(
          "The random-media service returned an invalid response.",
          true,
        );
      }

      let data;
      try {
        data = body ? JSON.parse(body) : null;
      } catch {
        throw requestProblem(
          "The random-media service returned unreadable data.",
          true,
        );
      }

      if (!response.ok) {
        const message =
          data && typeof data.error === "string"
            ? data.error
            : `The random-media service returned error ${response.status}.`;
        const problem = requestProblem(message, isTransientStatus(response.status));
        problem.noMatch = response.status === 404;
        throw problem;
      }

      if (
        !data ||
        typeof data.url !== "string" ||
        typeof data.ext !== "string" ||
        !Number.isFinite(Number(data.total))
      ) {
        throw requestProblem(
          "The random-media service returned incomplete data.",
          true,
        );
      }

      let mediaUrl;
      try {
        mediaUrl = new URL(data.url, window.location.href);
      } catch {
        throw requestProblem(
          "The random-media service returned an invalid media address.",
          true,
        );
      }
      if (
        mediaUrl.origin !== window.location.origin ||
        !mediaUrl.pathname.startsWith("/media/")
      ) {
        throw requestProblem(
          "The random-media service returned an unsafe media address.",
          true,
        );
      }

      return {
        ...data,
        url: `${mediaUrl.pathname}${mediaUrl.search}`,
      };
    } catch (problem) {
      if (problem && problem.retryable !== undefined) throw problem;
      if (problem && problem.name === "AbortError") {
        throw requestProblem("The random-media request timed out.", true);
      }
      throw requestProblem("The random-media service could not be reached.", true);
    } finally {
      window.clearTimeout(timeout);
    }
  }

  async function fetchRandom(extension, tags) {
    let lastProblem;
    for (let attempt = 1; attempt <= RANDOM_ATTEMPTS; attempt += 1) {
      try {
        return await fetchRandomOnce(extension, tags);
      } catch (problem) {
        lastProblem = problem;
        if (!problem.retryable || attempt === RANDOM_ATTEMPTS) throw problem;
        status.textContent = `Retrying… ${attempt + 1}/${RANDOM_ATTEMPTS}`;
        await wait(300 * attempt);
      }
    }
    throw lastProblem;
  }

  async function loadTagCatalog() {
    if (tagCatalogLoaded || tagCatalogLoading || mobileQuery.matches) return;
    tagCatalogLoading = true;
    try {
      const response = await fetch("/api/tags", {
        cache: "no-store",
        headers: { accept: "application/json" },
      });
      const data = await response.json();
      if (!response.ok || !data || !Array.isArray(data.tags)) {
        throw new Error("Tag catalog unavailable");
      }
      const fragment = document.createDocumentFragment();
      for (const entry of data.tags) {
        if (
          !entry
          || !Number.isSafeInteger(entry.id)
          || typeof entry.name !== "string"
          || !Number.isSafeInteger(entry.count)
        ) continue;
        const label = document.createElement("label");
        label.className = "tag-option";
        const checkbox = document.createElement("input");
        checkbox.type = "checkbox";
        checkbox.value = entry.name;
        const name = document.createElement("span");
        name.className = "tag-name";
        name.textContent = entry.name;
        const count = document.createElement("span");
        count.className = "tag-count";
        count.textContent = entry.count.toLocaleString();
        label.append(checkbox, name, count);
        fragment.appendChild(label);
      }
      tagList.replaceChildren(fragment);
      tagCatalogLoaded = true;
      tagSidebarState.textContent = data.tags.length
        ? ""
        : "Press TAG TIME on your PC to build the catalog.";
    } catch {
      tagSidebarState.textContent = "Tags are temporarily unavailable.";
    } finally {
      tagCatalogLoading = false;
    }
  }

  function intrinsicSize(media) {
    return media.tagName === "VIDEO"
      ? { width: media.videoWidth, height: media.videoHeight }
      : { width: media.naturalWidth, height: media.naturalHeight };
  }

  function applySizeMode(media, centerNative = false) {
    if (!media) return;
    if (mobileQuery.matches) {
      sizeMode = "fit";
      stage.classList.remove("native");
      media.style.removeProperty("width");
      media.style.removeProperty("height");
      stage.scrollLeft = 0;
      stage.scrollTop = 0;
      return;
    }

    const { width, height } = intrinsicSize(media);
    if (!(width > 0 && height > 0)) return;
    const native = sizeMode === "native";
    stage.classList.toggle("native", native);

    let renderedWidth = width;
    let renderedHeight = height;
    if (!native) {
      const scale = Math.min(
        1,
        Math.max(1, stage.clientWidth) / width,
        Math.max(1, stage.clientHeight) / height,
      );
      renderedWidth = Math.max(1, Math.round(width * scale));
      renderedHeight = Math.max(1, Math.round(height * scale));
    }

    media.style.width = `${renderedWidth}px`;
    media.style.height = `${renderedHeight}px`;
    requestAnimationFrame(() => {
      if (native && centerNative) {
        stage.scrollLeft = Math.max(0, (stage.scrollWidth - stage.clientWidth) / 2);
        stage.scrollTop = Math.max(0, (stage.scrollHeight - stage.clientHeight) / 2);
      } else if (!native) {
        stage.scrollLeft = 0;
        stage.scrollTop = 0;
      }
    });
  }

  function addHint() {
    if (!mobileQuery.matches || !showHint) return;
    const hint = document.createElement("div");
    hint.id = "hint";
    hint.textContent = "Tap the image to load the next random item";
    stage.appendChild(hint);
  }

  async function loadRandom() {
    if (loading) return;
    loading = true;
    next.disabled = true;
    error.style.display = "none";
    status.textContent = "Loading…";

    try {
      const activeTags = selectedTags();
      const data = await fetchRandom(filter.value, activeTags);

      const ext = String(data.ext || "").toLowerCase();
      const media = ["mp4", "m4v", "webm"].includes(ext)
        ? document.createElement("video")
        : document.createElement("img");

      if (media.tagName === "VIDEO") {
        media.autoplay = true;
        media.loop = true;
        media.muted = true;
        media.playsInline = true;
        media.preload = "auto";
        media.disablePictureInPicture = true;
        media.controls = false;
      } else {
        media.alt = "";
        media.decoding = "async";
      }

      media.id = "media";
      media.src = data.url;
      const mediaWrap = document.createElement("div");
      mediaWrap.id = "media-wrap";
      mediaWrap.appendChild(media);
      stage.replaceChildren(mediaWrap);
      addHint();
      stage.scrollLeft = 0;
      stage.scrollTop = 0;
      status.textContent = activeTags.length ? "Filtered" : `${data.total} matching`;

      const readyEvent = media.tagName === "VIDEO" ? "loadedmetadata" : "load";
      media.addEventListener(
        readyEvent,
        () => applySizeMode(media, sizeMode === "native"),
        { once: true },
      );
      if (
        (media.tagName === "IMG" && media.complete && media.naturalWidth > 0) ||
        (media.tagName === "VIDEO" && media.readyState >= 1 && media.videoWidth > 0)
      ) {
        applySizeMode(media, sizeMode === "native");
      }
      if (media.tagName === "VIDEO") media.play().catch(() => {});
    } catch (problem) {
      if (problem && problem.noMatch) {
        const noMatch = document.createElement("div");
        noMatch.id = "no-match";
        noMatch.textContent = "Nothing matches those tags.";
        stage.replaceChildren(noMatch);
        error.style.display = "none";
        status.textContent = "No match";
        return;
      }
      error.textContent = `${problem.message} Tap Next random to try again.`;
      error.style.display = "block";
      status.textContent = "Error";
    } finally {
      loading = false;
      next.disabled = false;
    }
  }


  function setSourceBusy(busy) {
    sourceInput.disabled = busy;
    sourceAdd.disabled = busy;
    sourceClose.disabled = busy;
    sourceAdd.textContent = busy ? "Adding…" : "Add";
  }

  function openSourceDialog() {
    sourceFeedback.textContent = "";
    sourceInput.value = "";
    sourceDialog.showModal();
    window.setTimeout(() => sourceInput.focus(), 0);
  }

  function closeSourceDialog() {
    if (sourceDialog.open) sourceDialog.close();
  }

  async function addManagedSource() {
    const subreddit = sourceInput.value.trim();
    if (!subreddit) {
      sourceFeedback.textContent = "Type a subreddit name first.";
      sourceInput.focus();
      return;
    }

    setSourceBusy(true);
    sourceFeedback.textContent = "";
    try {
      const response = await fetch("/api/sources", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers: {
          accept: "application/json",
          "content-type": "application/json",
          "x-gparty-source-request": "1",
        },
        body: JSON.stringify({ subreddit }),
      });
      const contentType = (response.headers.get("content-type") || "").toLowerCase();
      const responseBody = await response.text();
      if (!contentType.includes("application/json")) {
        throw new Error(
          response.ok
            ? "The source service returned an invalid response."
            : `Adding the subreddit failed with error ${response.status}.`,
        );
      }

      let data;
      try {
        data = responseBody ? JSON.parse(responseBody) : null;
      } catch {
        throw new Error("The source service returned unreadable data.");
      }
      if (!response.ok) {
        throw new Error(
          data && typeof data.error === "string"
            ? data.error
            : `Adding the subreddit failed with error ${response.status}.`,
        );
      }
      if (
        !data
        || typeof data.added !== "boolean"
        || typeof data.alreadyExists !== "boolean"
        || !Number.isFinite(Number(data.count))
      ) {
        throw new Error("The source service returned incomplete data.");
      }

      sourceInput.value = "";
      sourceFeedback.textContent = data.alreadyExists
        ? "That subreddit is already added."
        : "Added. Boink will use it next run.";
    } catch (problem) {
      sourceFeedback.textContent =
        problem && problem.message
          ? problem.message
          : "The subreddit could not be added.";
    } finally {
      setSourceBusy(false);
    }
  }

  next.addEventListener("click", loadRandom);
  addSourceOpen.addEventListener("click", openSourceDialog);
  sourceClose.addEventListener("click", closeSourceDialog);
  sourceForm.addEventListener("submit", (event) => {
    event.preventDefault();
    addManagedSource();
  });
  sourceDialog.addEventListener("click", (event) => {
    if (event.target === sourceDialog) closeSourceDialog();
  });
  filter.addEventListener("change", () => {
    showHint = false;
    loadRandom();
  });
  tagList.addEventListener("change", (event) => {
    if (!event.target.matches('input[type="checkbox"]')) return;
    showHint = false;
    loadRandom();
  });
  stage.addEventListener("click", (event) => {
    if (event.target.id !== "media") return;
    if (mobileQuery.matches) {
      showHint = false;
      loadRandom();
      return;
    }
    sizeMode = sizeMode === "fit" ? "native" : "fit";
    applySizeMode(event.target, sizeMode === "native");
  });

  function reapplyCurrentSize() {
    const media = document.getElementById("media");
    if (media) applySizeMode(media, false);
  }

  function scheduleSizeRefresh() {
    if (resizeFrame) window.cancelAnimationFrame(resizeFrame);
    resizeFrame = window.requestAnimationFrame(() => {
      resizeFrame = 0;
      reapplyCurrentSize();
    });
  }

  window.addEventListener("resize", scheduleSizeRefresh);
  window.addEventListener("orientationchange", scheduleSizeRefresh);
  window.addEventListener("pageshow", () => {
    setSourceBusy(false);
    scheduleSizeRefresh();
  });
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", scheduleSizeRefresh);
  }
  if ("ResizeObserver" in window) {
    new ResizeObserver(scheduleSizeRefresh).observe(stage);
  }
  mobileQuery.addEventListener("change", () => {
    scheduleSizeRefresh();
    if (!mobileQuery.matches) loadTagCatalog();
  });
  document.addEventListener("fullscreenchange", scheduleSizeRefresh);
  document.addEventListener("keydown", (event) => {
    if (sourceDialog.open) return;
    if (event.code === "Space") {
      event.preventDefault();
      showHint = false;
      loadRandom();
    } else if (event.key.toLowerCase() === "f") {
      if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
      else document.documentElement.requestFullscreen().catch(() => {});
    }
  });

  if (!mobileQuery.matches) loadTagCatalog();
  loadRandom();
})();
