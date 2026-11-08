// iroha ウェブサイトの動き
// 実演の変換結果は、iroha-kkc（と iroha-typo）に読みを 1 文字ずつ渡して得た実際の出力をそのまま並べている。
// 文を差し替えるときは iroha-cli で読みの先頭から 1 文字ずつ変換し直して、ここに写すこと。

(() => {
  "use strict";

  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---------- 論文のリンクと引用の書き方（Jxiv で公開されたら url と doi を入れる） ----------
  // url と doi のどちらかが入った論文だけ、札がリンクになり、「引用するとき」の欄に書き方が出る。
  // 例: url: "https://jxiv.jst.go.jp/index.php/jxiv/preprint/view/XXXX", doi: "10.51094/jxiv.XXXX", year: 2026
  const PAPERS = {
    kkc: {
      url: null,
      doi: null,
      year: 2026,
      key: "baba2026irohakkc",
      title: "自動変換に向けた浅いデコーダを持つ文字単位エンコーダ・デコーダモデルによるニューラルかな漢字変換の高速化",
    },
    typo: {
      url: null,
      doi: null,
      year: 2026,
      key: "baba2026irohatypo",
      title: "かな漢字変換の手前で読みを直すローマ字入力の打ち間違い訂正",
    },
  };
  const AUTHOR = { ja: "馬場 哲晃", bib: "馬場, 哲晃" };
  const paperURL = (p) => p.url || (p.doi ? `https://doi.org/${p.doi}` : null);

  document.querySelectorAll("[data-paper]").forEach((el) => {
    const url = paperURL(PAPERS[el.dataset.paper] || {});
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

  function setupCitations() {
    const box = document.getElementById("cite");
    const list = document.getElementById("cite-list");
    if (!box || !list) return;
    const published = Object.values(PAPERS).filter((p) => paperURL(p));
    if (published.length === 0) return;

    for (const p of published) {
      const link = p.doi ? `https://doi.org/${p.doi}` : p.url;
      const text = `${AUTHOR.ja}: ${p.title}, Jxiv プレプリント (${p.year}). ${link}`;
      const bib = [
        `@misc{${p.key},`,
        `  author    = {${AUTHOR.bib}},`,
        `  title     = {${p.title}},`,
        `  year      = {${p.year}},`,
        `  publisher = {Jxiv},`,
        `  note      = {プレプリント},`,
        p.doi ? `  doi       = {${p.doi}},` : null,
        `  url       = {${p.url || link}}`,
        `}`,
      ].filter(Boolean).join("\n");

      const item = document.createElement("div");
      item.className = "cite-item";
      const t = document.createElement("p");
      t.className = "cite-text";
      t.textContent = text;
      const pre = document.createElement("pre");
      pre.className = "cite-bib";
      pre.textContent = bib;
      const actions = document.createElement("div");
      actions.className = "cite-actions";
      actions.append(copyButton("書式をコピー", text), copyButton("BibTeX をコピー", bib));
      item.append(t, pre, actions);
      list.append(item);
    }
    box.hidden = false;
  }

  function copyButton(label, value) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "cite-copy";
    b.textContent = label;
    b.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(value);
        b.textContent = "コピーしました";
      } catch {
        b.textContent = "コピーできませんでした";
      }
      setTimeout(() => { b.textContent = label; }, 1800);
    });
    return b;
  }

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
  // 文全体を読んで変換する効果: あとから打った「プレゼント」を読んで、前の「送る」が「贈る」に変わる。
  // 途中の「プレゼン」では「送る」に戻る。iroha-kkc-v1（Q5_K_M）でユーザ辞書・学習なしの出力（2026-11-08）
  const OKURU = [
    ["ta", "た"], ["be", "食べ"], ["mo", "食べも"], ["no", "食べ物"], ["wo", "食べ物を"], ["o", "食べ物をお"], ["ku", "食べ物を置く"],
    ["ru", "食べ物を送る"], ["no", "食べ物を送るの"], ["ha", "食べ物を贈るのは"], ["yo", "食べ物を贈るのはよ"], ["ku", "食べ物を贈るのはよく"],
    ["na", "食べ物を送るのは良くな"], ["i", "食べ物を贈るのは良くない"], ["ka", "食べ物を贈るのはよくないか"], ["na", "食べ物を送るのは良くないかな"],
    ["?", "食べ物を送るのは良くないかな？"], ["ho", "食べ物を送るのは良くないかな？ほ"], ["ka", "食べ物を送るのは良くないかな？他"],
    ["ni", "食べ物を送るのは良くないかな？他に"], ["na", "食べ物を送るのは良くないかな？他にな"], ["nn", "食べ物を送るのは良くないかな？他に何"],
    ["no", "食べ物を送るのは良くないかな？他に何の"], ["pu", "食べ物を送るのは良くないかな？他になんのプ"], ["re", "食べ物を送るのは良くないかな？他に何のプレ"],
    ["ze", "食べ物を贈るのは良くないかな？他に何のプレゼ"], ["nn", "食べ物を送るのはよくないかな？他に何のプレゼン"], ["to", "食べ物を贈るのはよくないかな？他に何のプレゼント"],
    ["ga", "食べ物を贈るのはよくないかな？他に何のプレゼントが"], ["a", "食べ物を贈るのはよくないかな？他に何のプレゼントがあ"],
    ["ru", "食べ物を贈るのはよくないかな？他に何のプレゼントがある"], ["ka", "食べ物を贈るのはよくないかな？他に何のプレゼントがあるか"],
    ["na", "食べ物を贈るのはよくないかな？他に何のプレゼントがあるかな"], ["?", "食べ物を贈るのはよくないかな？他に何のプレゼントがあるかな？"],
  ];
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
      steps: OKURU,
      caption: "文全体を読んで変換します。あとから「プレゼント」と打つと、前の「送る」が「贈る」に変わります。",
    },
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
      v.done.textContent = OKURU[OKURU.length - 1][1];
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

  // ---------- 専門辞書 ----------
  // 例ごとに「辞書あり」「辞書なし」を流す。打つそばからの変換は辞書で変わらないので、Space（と Shift+→・↓）で
  // 候補ウィンドウを開いて違いを見せる。候補は iroha-kkc-v1（Q5_K_M）・ユーザ辞書と学習なしで、その専門辞書の有無を
  // iroha-cli convert --n で出した上位 5 件。打ちかけの変換は iroha-cli に読みの頭から 1 字ずつ渡した結果。
  // 工学の例の辞書ありの並びは、IME で録画した候補ウィンドウ（website/raw/5-term.mp4）と同じ。
  // 読みが 1 つの文節になる例は、Space で候補ウィンドウが開き、1 番目の候補が入る（IME と同じ）
  const TERMS_SAMPLES = [
    {
      label: "工学",
      reading: "なんどかいろをくみあわせる",
      typed: [
        ["na", "な"], ["nn", "何"], ["do", "何度"], ["ka", "何度か"], ["i", "何度かい"], ["ro", "何度か色"],
        ["wo", "何度か色を"], ["ku", "何度か色をく"], ["mi", "何度か色を組み"], ["a", "何度か色を組みあ"],
        ["wa", "何度か色を組み合わ"], ["se", "何度か色を組み合わせ"], ["ru", "何度か色を組み合わせる"],
      ],
      live: "何度か色を",
      rest: "を組み合わせる",
      // 候補ウィンドウを開くまでの文節の操作: [キー, 説明, 選んでいる文節, 後ろの文節]
      prep: [
        ["space", "文節に分ける", "何度か", "色を組み合わせる"],
        ["shift + →", "2 回押して、文節を「なんどかいろ」まで伸ばす", "何度か色", "を組み合わせる"],
      ],
      open: ["↓", "候補を開く"],
      term: "NAND回路",
      dict: { candidates: [["何度か色"], ["NAND回路", "工学"], ["何度かいろ"], ["何度回路"], ["難度回路"]], pick: 1 },
      nodict: [["何度か色"], ["何度カイロ"], ["何度かいろ"], ["何度回路"], ["何度海路"]],
    },
    {
      label: "薬の名前",
      reading: "るふぃなみどじょう",
      typed: [
        ["ru", "る"], ["fi", "ルフィ"], ["na", "ルフィナ"], ["mi", "ルフィナミ"], ["do", "ルフィナミド"],
        ["jo", "ルフィナミドジョ"], ["u", "ルフィナミドジョウ"],
      ],
      live: "ルフィナミドジョウ",
      rest: "",
      prep: [],
      open: ["space", "候補を開く"],
      term: "ルフィナミド錠",
      dict: { candidates: [["ルフィナミド錠", "薬の名前"], ["ルフィナミド城"], ["ルフィナミド上"], ["ルフィナミド状"], ["ルフィナミド丈"]], pick: 0 },
      nodict: [["ルフィ並み土壌"], ["ルフィ並み度上"], ["ルフィ並み度条"], ["ルフィ並み度場"], ["ルフィ並み度帖"]],
    },
    {
      label: "駅",
      reading: "のぞきえき",
      typed: [["no", "の"], ["zo", "のぞ"], ["ki", "除き"], ["e", "覗え"], ["ki", "除駅"]],
      live: "除駅",
      rest: "",
      prep: [],
      open: ["space", "候補を開く"],
      term: "及位駅",
      dict: { candidates: [["及位駅", "駅"], ["除駅"], ["除き液"], ["覗駅"], ["及位益"]], pick: 0 },
      nodict: [["除駅"], ["除き液"], ["覗駅"], ["のぞき液"], ["除き益"]],
    },
    {
      label: "数学・統計",
      reading: "ししょくもんだい",
      typed: [
        ["shi", "し"], ["shi", "獅子"], ["yo", "支所"], ["ku", "試食"], ["mo", "試食も"], ["nn", "試食文"],
        ["da", "試食もんだ"], ["i", "試食問題"],
      ],
      live: "試食問題",
      rest: "",
      prep: [],
      open: ["space", "候補を開く"],
      term: "四色問題",
      dict: { candidates: [["試食問題"], ["四色問題", "数学・統計"], ["史色問題"], ["死色問題"], ["試食門第"]], pick: 1 },
      nodict: [["試食問題"], ["市色問題"], ["史色問題"], ["死色問題"], ["試食門第"]],
    },
  ];

  function runTerms() {
    const root = document.getElementById("terms-demo");
    if (!root) return;
    const v = demoView(root);
    const list = root.querySelector(".cand-window");
    const status = document.querySelector("#terms .demo-status");
    const samplePicker = document.querySelector("#terms .terms-samples");
    const dictPicker = document.querySelector("#terms .terms-dict");
    let token = 0;
    let sample = 0;
    let withDict = true;

    const button = (parent, text, onClick) => {
      const b = document.createElement("button");
      b.type = "button";
      b.textContent = text;
      b.addEventListener("click", onClick);
      parent.appendChild(b);
      return b;
    };
    const sampleButtons = TERMS_SAMPLES.map((ex, idx) => button(samplePicker, ex.label, () => { sample = idx; loop(); }));
    const dictButtons = [true, false].map((d) => button(dictPicker, d ? "辞書あり" : "辞書なし", () => { withDict = d; loop(); }));

    function segments(first, rest) {
      v.composing.innerHTML = "";
      const a = document.createElement("span"); a.className = "seg sel"; a.textContent = first;
      v.composing.append(a);
      if (rest) { const b = document.createElement("span"); b.className = "seg"; b.textContent = rest; v.composing.append(b); }
    }

    function showList(cands, selected) {
      list.innerHTML = "";
      cands.forEach(([text, tag], i) => {
        const li = document.createElement("li");
        if (i === selected) li.className = "sel";
        const n = document.createElement("span"); n.className = "n"; n.textContent = String(i + 1);
        const t = document.createElement("span"); t.textContent = text;
        li.append(n, t);
        if (tag) { const g = document.createElement("span"); g.className = "tag"; g.textContent = tag; li.append(g); }
        list.append(li);
      });
      list.classList.add("show");
    }

    function reset() {
      v.done.textContent = "";
      v.composing.textContent = "";
      v.setKey("");
      list.classList.remove("show");
      status.textContent = "";
    }

    const cands = (ex, d) => (d ? ex.dict.candidates : ex.nodict);
    const found = (ex, d) => (d
      ? `${ex.label}の専門辞書から「${ex.term}」が候補に出ました。`
      : `辞書がないと、候補に「${ex.term}」は出てきません。`);

    function showResult(ex, d) {
      const pick = d ? ex.dict.pick : 0;
      segments(cands(ex, d)[pick][0], ex.rest);
      showList(cands(ex, d), pick);
      status.textContent = found(ex, d);
    }

    async function play(ex, d, my) {
      const alive = () => my === token;
      const c = cands(ex, d);
      const pick = d ? ex.dict.pick : -1;
      reset();
      status.textContent = `「${ex.reading}」と打つと…`;
      await wait(600);
      for (const [key, shown] of ex.typed) {
        if (!alive()) return false;
        v.setKey(key);
        v.composing.textContent = shown;
        await wait(150 + key.length * 50);
      }
      if (!alive()) return false;
      status.textContent = `打つそばからの変換では「${ex.live}」になりました。`;
      await wait(1800);
      for (const [key, note, first, rest] of ex.prep) {
        if (!alive()) return false;
        v.setKey(key, note);
        segments(first, rest);
        await wait(1400);
      }
      if (!alive()) return false;
      v.setKey(ex.open[0], ex.open[1]);
      segments(c[0][0], ex.rest);
      showList(c, 0);
      status.textContent = found(ex, d);
      await wait(1400);
      for (let i = 1; i <= pick; i++) {
        if (!alive()) return false;
        v.setKey("↓");
        showList(c, i);
        segments(c[i][0], ex.rest); // IME と同じく、選んだ候補が文節に入る
        await wait(900);
      }
      if (!alive()) return false;
      await wait(pick >= 0 ? 1200 : 2600);
      if (!alive()) return false;
      list.classList.remove("show");
      if (pick >= 0) {
        v.setKey("return");
        v.done.textContent = c[pick][0] + ex.rest;
        v.composing.textContent = "";
        await wait(2400);
      } else {
        v.setKey("esc");
        v.composing.textContent = ex.typed[ex.typed.length - 1][1];
        await wait(1400);
      }
      return alive();
    }

    // 自動では 例 1 の辞書あり → 例 1 の辞書なし → 例 2 の辞書あり → … の順に流す
    async function loop() {
      const my = ++token;
      for (;;) {
        sampleButtons.forEach((b, idx) => b.setAttribute("aria-pressed", String(idx === sample)));
        dictButtons.forEach((b, idx) => b.setAttribute("aria-pressed", String((idx === 0) === withDict)));
        const ex = TERMS_SAMPLES[sample];
        if (reduceMotion) { reset(); showResult(ex, withDict); return; }
        const ok = await play(ex, withDict, my);
        if (!ok) return;
        if (withDict) withDict = false;
        else { withDict = true; sample = (sample + 1) % TERMS_SAMPLES.length; }
      }
    }

    if (!("IntersectionObserver" in window)) return loop();
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting)) { loop(); io.disconnect(); }
    }, { threshold: 0.3 });
    io.observe(root);
  }

  // ---------- 開発者モードの説明（切り替えで出す。ページを開き直すと閉じた状態に戻る） ----------
  function setupDevmode() {
    const toggle = document.getElementById("devmode-toggle");
    const body = document.getElementById("devmode-body");
    if (!toggle || !body) return;
    toggle.addEventListener("click", () => {
      const on = toggle.getAttribute("aria-checked") !== "true";
      toggle.setAttribute("aria-checked", String(on));
      body.hidden = !on;
    });
  }

  runHero();
  runSpeed();
  runTypo();
  runTerms();
  setupDevmode();
  setupCitations();
})();
