// iroha ウェブサイトの動き
// 実演の変換結果は、iroha-kkc（と iroha-typo）に読みを 1 文字ずつ渡して得た実際の出力をそのまま並べている。
// 文を差し替えるときは iroha-cli で読みの先頭から 1 文字ずつ変換し直して、ここに写すこと。

(() => {
  "use strict";

  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---------- 論文のリンク（公開されたらここに URL を入れる） ----------
  const PAPERS = {
    kkc: null,  // 例: "https://jxiv.jst.go.jp/index.php/jxiv/preprint/view/XXXX"
    typo: null,
  };
  document.querySelectorAll("[data-paper]").forEach((el) => {
    const url = PAPERS[el.dataset.paper];
    if (!url) {
      el.addEventListener("click", (e) => e.preventDefault());
      return;
    }
    el.href = url;
    el.classList.remove("pending");
    el.removeAttribute("aria-disabled");
    const status = el.querySelector(".status");
    if (status) status.textContent = "Jxiv で読む";
  });

  // ---------- テキストの窓の表示 ----------
  function demoView(root) {
    return {
      done: root.querySelector(".done"),
      composing: root.querySelector(".composing"),
      keys: root.querySelector(".keys"),
      dots: root.querySelector(".pause-dots"),
      popup: root.querySelector(".typo-popup"),
      setKey(text, note) {
        this.keys.innerHTML = "";
        if (!text) return;
        const k = document.createElement("span");
        k.className = "key typed";
        k.textContent = text;
        this.keys.appendChild(k);
        if (note) {
          const n = document.createElement("span");
          n.className = "key-note";
          n.textContent = note;
          this.keys.appendChild(n);
        }
      },
    };
  }

  // 1 打ちごとの [打ったキー, その時点のライブ変換の表示]
  // 文全体を読んで変換する効果: 最後の「た」で、前の「記者」が「汽車」「帰社」に変わる
  const KISHA = [
    ["ki", "き"], ["sha", "記者"], ["ga", "記者が"], ["ki", "記者が来"], ["sha", "記者が記者"], ["de", "記者が記者で"],
    ["ki", "記者が記者でき"], ["sha", "記者が記者で記者"], ["shi", "記者が記者で記者し"], ["ta", "記者が汽車で帰社した"],
  ];
  // 打ち間違いの訂正: 「確認（kakunin）」を「kakuin」と打った例（実際に起きた打ち間違い）
  const KAKUIN = [
    ["ka", "か"], ["i", "貝"], ["gi", "会議"], ["no", "会議の"], ["shi", "会議のし"], ["ryo", "会議の思慮"],
    ["u", "会議の資料"], ["wo", "会議の資料を"], ["ka", "会議の資料を下"], ["ku", "会議の資料を各"],
    ["i", "会議の資料を各位"], ["n", "会議の資料を各員"], ["shi", "会議の資料を各印紙"], ["ma", "会議の資料を客員しま"],
    ["shi", "会議の資料を各印しまし"], ["ta", "会議の資料を客員しました"],
  ];
  // 打ち間違いの訂正の章で使う、正しく打った文
  const KAISATSU = [
    ["ka", "か"], ["i", "貝"], ["sa", "介さ"], ["tsu", "改札"], ["no", "改札の"], ["ma", "改札の間"],
    ["e", "改札の前"], ["de", "改札の前で"], ["cho", "改札の前でちょ"], ["t", "改札の前でちょっ"],
    ["to", "改札の前でちょっと"], ["ma", "改札の前でちょっとま"], ["t", "改札の前でちょっと待っ"],
    ["te", "改札の前でちょっと待って"], ["te", "改札の前でちょっと待ってて"],
  ];
  // AI 変換（⌥ + Return で英訳）の例。英訳の文は使う AI サービスで変わるので、README の例と同じ文にしてある
  const KAIGI_AI = [
    ["a", "あ"], ["shi", "足"], ["ta", "明日"], ["ka", "アシタカ"], ["i", "明日書い"], ["gi", "あした会議"],
    ["ya", "明日会議や"], ["ru", "明日会議やる"], ["ka", "明日会議やるか"], ["ra", "明日会議やるから"],
    ["ki", "明日会議やるから来"], ["te", "明日会議やるから来て"],
  ];
  const HERO_SENTENCES = [
    {
      steps: KISHA,
      caption: "文全体を読んで変換します。最後まで打つと、前の「記者」が「汽車」「帰社」に変わります。",
    },
    {
      steps: KAKUIN,
      typo: { from: "かいぎのしりょうをかくいんしました", to: "かいぎのしりょうをかくにんしました", shown: "会議の資料を確認しました" },
      caption: "「確認」を「かくいん」と打ち間違えても、手が止まったときに直してから変換します。",
    },
    {
      steps: KAIGI_AI,
      ai: { key: "⌥ + return", label: "AI で英訳", result: "Come to the meeting tomorrow." },
      caption: "⌥ + Return で、打った日本語を AI に英訳してもらえます。",
    },
  ];

  async function runHero() {
    const root = document.getElementById("hero-demo");
    if (!root) return;
    const v = demoView(root);
    const caption = document.getElementById("hero-caption");
    if (reduceMotion) {
      v.done.textContent = "記者が汽車で帰社した";
      caption.textContent = HERO_SENTENCES[0].caption;
      return;
    }
    let i = 0;
    for (;;) {
      const { steps, ai, typo, caption: text } = HERO_SENTENCES[i % HERO_SENTENCES.length];
      v.done.textContent = "";
      v.composing.textContent = "";
      v.setKey("");
      caption.textContent = text;
      await wait(900);
      for (const [key, shown] of steps) {
        v.setKey(key);
        v.composing.textContent = shown;
        await wait(170 + key.length * 55);
      }
      await wait(900);
      if (typo) {
        // 手が止まる → 読みを直して変換し直す → カーソルの下に知らせる
        v.setKey("");
        v.dots.classList.add("show");
        await wait(1100);
        v.dots.classList.remove("show");
        v.composing.textContent = typo.shown;
        v.popup.innerHTML = "";
        const s = document.createElement("s"); s.textContent = typo.from;
        const b = document.createElement("b"); b.textContent = typo.to;
        v.popup.append(s, " → ", b);
        v.popup.classList.add("show");
        await wait(3200);
        v.popup.classList.remove("show");
        v.setKey("return");
      } else if (ai) {
        // AI の結果は届いたそばから表示される。英語の結果は Return を待たずにそのまま確定する
        v.setKey(ai.key, ai.label);
        await wait(700);
        v.composing.textContent = "";
        for (const word of ai.result.split(/(?<= )/)) {
          v.composing.textContent += word;
          await wait(110);
        }
        await wait(250);
      } else {
        await wait(900);
        v.setKey("return");
      }
      v.done.textContent = v.composing.textContent;
      v.composing.textContent = "";
      await wait(2800);
      i += 1;
    }
  }

  // ---------- 速さと正確さ ----------
  // 速さと正確さのグラフ: 見える位置に来たら点と矢印を順に出す
  function runSpeed() {
    const box = document.getElementById("speed");
    if (!box) return;
    function play() {
      box.classList.remove("play");
      void box.offsetWidth;
      box.classList.add("play");
    }
    box.querySelector("[data-replay]")?.addEventListener("click", play);
    if (reduceMotion || !("IntersectionObserver" in window)) return box.classList.add("play");
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting)) { play(); io.disconnect(); }
    }, { threshold: 0.4 });
    io.observe(box);
  }

  // ---------- 打ち間違いの訂正 ----------
  // typed: 打鍵と、その時点のライブ変換の表示。fix: 手が止まったときの訂正（読みの前後と、直した読みの変換）。fix が無い例は正しく打った文
  const TYPO_EXAMPLES = [
    {
      label: "jっこうしました",
      typed: [["j", "j"], ["k", "jっ"], ["ko", "jっ子"], ["u", "jっこう"], ["shi", "jっ講師"], ["ma", "jっこう島"],
        ["shi", "jっこうしまし"], ["ta", "jっこうしました"]],
      fix: { from: "jっこうしました", to: "じっこうしました", shown: "実行しました" },
      note: "「jikkou」の i が抜けた打ち間違い",
    },
    {
      label: "おねがいsます",
      typed: [["o", "お"], ["ne", "尾根"], ["ga", "尾根が"], ["i", "お願い"], ["s", "お願いs"], ["ma", "お願いsま"],
        ["su", "お願sます"]],
      fix: { from: "おねがいsます", to: "おねがいします", shown: "お願いします" },
      note: "「si」の i が抜けた打ち間違い",
    },
    {
      label: "ありがとうございmす",
      typed: [["a", "あ"], ["ri", "あり"], ["ga", "ありが"], ["to", "ありがと"], ["u", "ありがとう"], ["go", "ありがとうご"],
        ["za", "ありがとうござ"], ["i", "ありがとうござい"], ["m", "ありがとうございm"], ["su", "ありがとうございmす"]],
      fix: { from: "ありがとうございmす", to: "ありがとうございます", shown: "ありがとうございます" },
      note: "「ma」の a が抜けた打ち間違い",
    },
    {
      label: "正しく打った文",
      typed: KAISATSU,
      fix: null,
      note: "打ち間違いのない文",
    },
  ];

  function runTypo() {
    const root = document.getElementById("typo-demo");
    if (!root) return;
    const v = demoView(root);
    const status = document.querySelector("#typo .demo-status");
    const picker = document.querySelector("#typo .typo-steps");
    let token = 0;
    let current = 0;

    const buttons = TYPO_EXAMPLES.map((ex, idx) => {
      const b = document.createElement("button");
      b.type = "button";
      b.textContent = ex.label;
      b.addEventListener("click", () => { current = idx; loop(); });
      picker.appendChild(b);
      return b;
    });

    function reset() {
      v.done.textContent = "";
      v.composing.textContent = "";
      v.setKey("");
      v.dots.classList.remove("show");
      v.popup.classList.remove("show");
      status.textContent = "";
    }

    function showFixed(ex) {
      v.composing.textContent = ex.fix ? ex.fix.shown : ex.typed[ex.typed.length - 1][1];
      if (ex.fix) {
        v.popup.innerHTML = "";
        const s = document.createElement("s"); s.textContent = ex.fix.from;
        const b = document.createElement("b"); b.textContent = ex.fix.to;
        v.popup.append(s, " → ", b);
        v.popup.classList.add("show");
        status.innerHTML = "";
        const strong = document.createElement("b"); strong.textContent = "直しました。";
        status.append(strong, " このすぐ後に Backspace を押すと元に戻ります。");
      } else {
        status.textContent = "正しく打った文なので、そのままにします。";
      }
    }

    async function play(ex, my) {
      reset();
      status.textContent = ex.note;
      await wait(600);
      for (const [key, shown] of ex.typed) {
        if (my !== token) return false;
        v.setKey(key);
        v.composing.textContent = shown;
        await wait(150 + key.length * 50);
      }
      if (my !== token) return false;
      v.setKey("");
      v.dots.classList.add("show");
      status.textContent = "手が止まりました…";
      await wait(1100);
      if (my !== token) return false;
      v.dots.classList.remove("show");
      showFixed(ex);
      await wait(3600);
      if (my !== token) return false;
      v.popup.classList.remove("show");
      v.setKey("return");
      v.done.textContent = v.composing.textContent;
      v.composing.textContent = "";
      await wait(1400);
      return my === token;
    }

    async function loop() {
      const my = ++token;
      for (;;) {
        buttons.forEach((b, idx) => b.setAttribute("aria-pressed", String(idx === current)));
        const ex = TYPO_EXAMPLES[current];
        if (reduceMotion) { reset(); showFixed(ex); return; }
        const ok = await play(ex, my);
        if (!ok) return;
        current = (current + 1) % TYPO_EXAMPLES.length;
      }
    }

    // 見える位置に来てから動かす
    if (!("IntersectionObserver" in window)) return loop();
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting)) { loop(); io.disconnect(); }
    }, { threshold: 0.3 });
    io.observe(root);
  }

  runHero();
  runSpeed();
  runTypo();
})();
