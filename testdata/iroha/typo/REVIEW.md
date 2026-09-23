# typo ベンチマーク 確認用一覧

`bench.tsv` から `make_bench.py` が作る（手で直さない）。直すときは bench.tsv を編集して作り直す。

## Removal（21 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| sub01 | 今日は楽しかった | きょうはたのしかった | きょはたのしかった | `kyohatanoshikatta` | deletion | kyou の u が抜ける（ょう が ょ になる）   |
| del01 | ありがとうございます | ありがとうございます | ありがとうgざいます | `arigatougzaimasu` | deletion | o が抜ける   |
| del02 | 写真送るね | しゃしんおくるね | しゃしのくるね | `shashinokurune` | deletion | ん の n が 1 つ（n+o が の になる）   |
| del03 | 今度ご飯行こう | こんどごはんいこう | こんどごはにこう | `konndogohanikou` | deletion | ん の n が 1 つ（n+i が に）   |
| del04 | 締め切りは金曜です | しめきりはきんようです | しめきりはきにょうです | `shimekirihakinyoudesu` | deletion | ん の n が 1 つ（n+yo が にょ）   |
| del05 | 実験の結果 | じっけんのけっか | じっけんおけっか | `jikkennokekka` | deletion | nn の後の n が抜ける（ん の後の の が お）  正解が学習にある |
| del06 | 駅前のパン屋 | えきまえのぱんや | えきまえのぱにゃ | `ekimaenopanya` | deletion | ん の n が 1 つ（n+ya が にゃ）   |
| del07 | 簡易的な方法 | かんいてきなほうほう | かにいてきなほうほう | `kaniitekinahouhou` | deletion | ん の n が 1 つ（n+i が に）   |
| del08 | 打ち合わせの日程 | うちあわせのにってい | うちあわせのにてい | `uchiawasenonitei` | missing_double_consonant | 促音の t が 1 つ   |
| del09 | 学校に行った | がっこうにいった | がこうにいった | `gakouniitta` | missing_double_consonant | 促音の k が 1 つ  正解が学習にある |
| del10 | やっぱりそうか | やっぱりそうか | やぱりそうか | `yaparisouka` | missing_double_consonant | 促音の p が 1 つ  正解が学習にある |
| del11 | 切符を買った | きっぷをかった | きぷをかった | `kipuwokatta` | missing_double_consonant | 促音の p が 1 つ   |
| del12 | 一緒に行こう | いっしょにいこう | いしょにいこう | `ishoniikou` | missing_double_consonant | 促音の s が 1 つ  正解が学習にある |
| del13 | 空港まで | くうこうまで | くこうまで | `kukoumade` | deletion | u が抜ける   |
| del14 | 映画館で見た | えいがかんでみた | えいあかんでみた | `eiakanndemita` | deletion | g が抜ける  正解が学習にある |
| del15 | 今日の予定 | きょうのよてい | きょうのおてい | `kyounootei` | deletion | y が抜ける   |
| del16 | 旅行の準備 | りょこうのじゅんび | ろこうのじゅんび | `rokounojunnbi` | deletion | y が抜ける   |
| del17 | 聞いてください | きいてください | きいてくあさい | `kiitekuasai` | deletion | d が抜ける   |
| del18 | 分かりました | わかりました | わかrました | `wakarmashita` | deletion | ri の i が抜ける（r だと わかい になり 若い と区別できない）   |
| del19 | 考えておきます | かんがえておきます | かんあえておきます | `kannaeteokimasu` | deletion | g が抜ける   |
| del20 | 仮説を検証する | かせつをけんしょうする | かせtsをけんしょうする | `kasetswokennshousuru` | deletion | tsu の u が抜ける   |

## Replacement（隣接キー）（59 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| sub02 | ちょっと待ってて | ちょっとまってて | ちっとまってて | `chittomattete` | substitution | o→i  正解が学習にある |
| sub03 | 今から行くね | いまからいくね | いまからいきね | `imakaraikine` | substitution | u→i（論文の I/U）   |
| sub04 | 大丈夫だよ | だいじょうぶだよ | だいじうぶだよ | `daijiubudayo` | substitution | o→i   |
| sub05 | 何時に集合する | なんじにしゅうごうする | なんじにしゅうごうづる | `nannjinishuugouduru` | substitution | s→d   |
| sub06 | 週末空いてる | しゅうまつあいてる | しゅうまrすあいてる | `shuumarsuaiteru` | substitution | t→r   |
| sub07 | めっちゃ眠い | めっちゃねむい | めっちゃねぬい | `mecchanenui` | substitution | m→n   |
| sub08 | 風邪ひいたかも | かぜひいたかも | かぇひいたかも | `kaxehiitakamo` | substitution | z→x（xe が小さい ぇ になる）   |
| sub09 | 返信遅れてごめん | へんしんおくれてごめん | へんしんおくててごめん | `hennshinnokutetegomenn` | substitution | r→t   |
| sub10 | お世話になっております | おせわになっております | おsrわになっております | `osrwaninatteorimasu` | substitution | e→r  正解が学習にある |
| sub11 | 資料を添付しました | しりょうをてんぷしました | しりょうをてんぷしmっした | `shiryouwotennpushimsshita` | substitution | a→s（ss が促音になる）   |
| sub12 | よろしくお願いします | よろしくおねがいします | よろしくおねgしします | `yoroshikuonegsishimasu` | substitution | a→s   |
| sub13 | 承知いたしました | しょうちいたしました | しょうちいたしまsぎた | `shouchiitashimasgita` | substitution | h→g  正解が学習にある |
| sub14 | 恐れ入りますが | おそれいりますが | おそえいりますが | `osoeirimasuga` | substitution | r→e  正解が学習にある |
| sub15 | 至急ご対応ください | しきゅうごたいおうください | しkつうごたいおうください | `shiktuugotaioukudasai` | substitution | y→t   |
| sub16 | 見積書を送付します | みつもりしょをそうふします | みつもりしょをそうぐします | `mitsumorishowosougushimasu` | substitution | f→g   |
| sub17 | 折り返しご連絡します | おりかえしごれんらくします | おりぁえしごれんらくします | `orilaeshigorennrakushimasu` | substitution | k→l（la が小さい ぁ になる）   |
| sub18 | 新しいカフェに行ってきた | あたらしいかふぇにいってきた | あたらしゅいかふぇにいってきた | `atarashuikafeniittekita` | substitution | i→u   |
| sub19 | 電車が遅れてる | でんしゃがおくれてる | でんshsがおくれてる | `dennshsgaokureteru` | substitution | a→s   |
| sub20 | 朝ごはん食べた | あさごはんたべた | あさごはんたねた | `asagohanntaneta` | substitution | b→n   |
| sub21 | 本研究では | ほんけんきゅうでは | ほんけんくううでは | `honnkennkuuudeha` | substitution | y→u   |
| sub22 | 先行研究によれば | せんこうけんきゅうによれば | せんこうけんきゅうによええば | `sennkoukennkyuuniyoeeba` | substitution | r→e  正解が学習にある |
| sub23 | 有意な差が見られた | ゆういなさがみられた | ゆういなさがもられた | `yuuinasagamorareta` | substitution | i→o（もられた は語としてある）   |
| sub24 | 統計的に | とうけいてきに | とうけいいぇきに | `toukeiyekini` | substitution | t→y   |
| sub25 | 被験者の数 | ひけんしゃのかず | ひけんしゃのかあう | `hikennshanokaau` | substitution | z→a  正解が学習にある |
| sub26 | 図に示すように | ずにしめすように | ずにしねすように | `zunishinesuyouni` | substitution | m→n   |
| sub27 | 社長に報告 | しゃちょうにほうこく | しゃちうにほうこく | `shachiunihoukoku` | substitution | o→i   |
| sub28 | 急いでください | いそいでください | いそいできださい | `isoidekidasai` | substitution | u→i   |
| sub29 | 普通に考えて | ふつうにかんがえて | ふぃつうにかんがえて | `fitsuunikanngaete` | substitution | u→i（fi が ふぃ になる）   |
| sub30 | 思います | おもいます | おみいます | `omiimasu` | substitution | o→i   |
| sub31 | 仕事終わった | しごとおわった | しぎとおわった | `shigitoowatta` | substitution | o→i   |
| sub32 | どこにいるの | どこにいるの | ぢこにいるの | `dikoniiruno` | substitution | o→i（di が ぢ になる）  正解が学習にある |
| sub33 | 足りないかも | たりないかも | tsりないかも | `tsrinaikamo` | substitution | a→s   |
| sub34 | 手紙を書いた | てがみをかいた | trがみをかいた | `trgamiwokaita` | substitution | e→r  正解が学習にある |
| sub35 | 聞こえない | きこえない | きぉえない | `kiloenai` | substitution | k→l（lo が ぉ になる）   |
| sub36 | 地下鉄で | ちかてつで | ちじゃてつで | `chijatetsude` | substitution | k→j  正解が学習にある |
| sub37 | 始めます | はじめます | がじめます | `gajimemasu` | substitution | h→g   |
| sub38 | 何もない | なにもない | なにのない | `naninonai` | substitution | m→n   |
| sub39 | 取り組み | とりくみ | とちくみ | `totikumi` | substitution | r→t（論文の R/T）  正解が学習にある |
| sub40 | 帰りました | かえりました | かえちました | `kaetimashita` | substitution | r→t  正解が学習にある |
| sub41 | 約束の時間 | やくそくのじかん | たくそくのじかん | `takusokunojikann` | substitution | y→t   |
| sub42 | 私は行かない | わたしはいかない | くぁたしはいかない | `qatashihaikanai` | substitution | w→q（qa が くぁ になる）   |
| sub43 | 勉強中です | べんきょうちゅうです | ねんきょうちゅうです | `nennkyouchuudesu` | substitution | b→n   |
| sub44 | 期待してる | きたいしてる | k8たいしてる | `k8taishiteru` | substitution | i→8（数字の段）  正解が学習にある |
| sub45 | 待ち合わせしよう | まちあわせしよう | まちあわswしよう | `machiawaswshiyou` | substitution | e→w   |
| sub46 | 天気予報 | てんきよほう | てんきよじょう | `tennkiyojou` | substitution | h→j  正解が学習にある |
| sub47 | 駅前で | えきまえで | wきまえで | `wkimaede` | substitution | e→w（先頭）  正解が学習にある |
| sub48 | 楽しみにしてる | たのしみにしてる | たのしみにしてり | `tanoshiminishiteri` | substitution | u→i（末尾）  正解が学習にある |
| sub49 | お疲れさま | おつかれさま | おつかてさま | `otsukatesama` | substitution | r→t  正解が学習にある |
| sub50 | ごめんね | ごめんね | ごめんへ | `gomennhe` | substitution | n→h   |
| sub51 | 会議の資料 | かいぎのしりょう | かいぎのsじりょう | `kaiginosjiryou` | substitution | h→j   |
| sub52 | 映画を観に行く | えいがをみにいく | えいがをみにいじゅ | `eigawominiiju` | substitution | k→j  正解が学習にある |
| sub53 | 準備ができた | じゅんびができた | じゅんびができら | `junnbigadekira` | substitution | t→r  正解が学習にある |
| sub54 | 明日は休み | あしたはやすみ | あしたはやすに | `ashitahayasuni` | substitution | m→n   |
| sub55 | 冷蔵庫に入れて | れいぞうこにいれて | れいぞうぉにいれて | `reizouloniirete` | substitution | k→l   |
| sub56 | 買ってきてほしい | かってきてほしい | かってきてじょしい | `kattekitejoshii` | substitution | h→j   |
| sub57 | ご都合いかがですか | ごつごういかがですか | ごつごういぁがですか | `gotsugouilagadesuka` | substitution | k→l   |
| sub58 | 年末年始 | ねんまつねんし | ねんなつねんし | `nennnatsunennshi` | substitution | m→n  正解が学習にある |
| sub59 | 電話してもいい | でんわしてもいい | でんえあしてもいい | `denneashitemoii` | substitution | w→e   |
| sub60 | 気をつけてね | きをつけてね | きをつけいぇね | `kiwotsukeyene` | substitution | t→y  正解が学習にある |

## Replacement（離れたキー）（14 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| far01 | 了解です | りょうかいです | りゅうかいです | `ryuukaidesu` | key_far | o→u（論文 E: ょ/ゅ の取り違え）   |
| far02 | 気をつけて帰ってね | きをつけてかえってね | きのつけてかえってね | `kinotsuketekaettene` | key_far | w→n（を→の。JWTD で最多）   |
| far03 | 部屋が寒い | へやがさむい | へやかさむい | `heyakasamui` | key_far | g→k（が→か）   |
| far04 | 天気がいいから散歩しよう | てんきがいいからさんぽしよう | てんきがいいからさんぼしよう | `tennkigaiikarasannboshiyou` | key_far | p→b   |
| far05 | 分析を行った | ぶんせきをおこなった | ぶんせきのおこなった | `bunnsekinookonatta` | key_far | w→n（を→の）  正解が学習にある |
| far06 | 会社に行く | かいしゃにいく | かいしょにいく | `kaishoniiku` | key_far | a→o（論文 H: SHA/SHO）   |
| far07 | 話したかった | はなしたかった | ほなしたかった | `honashitakatta` | key_far | a→o（論文 H: A/O）  正解が学習にある |
| far08 | 準備中 | じゅんびちゅう | じょんびちゅう | `jonnbichuu` | key_far | u→o（論文 E: U/O）  正解が学習にある |
| far09 | 表示されない | ひょうじされない | ひゅうじされない | `hyuujisarenai` | key_far | o→u  正解が学習にある |
| far10 | 少しだけ | すこしだけ | すそしだけ | `susoshidake` | key_far | k→s（論文 A: K/S）   |
| far11 | 何にする | なににする | ないいにする | `naiinisuru` | key_far | n→i（論文 A: N/I）   |
| far12 | 詳しいことは後で説明します | くわしいことはあとでせつめいします | くわしいことはあとてせつめいします | `kuwashiikotohaatotesetsumeishimasu` | key_far | d→t   |
| far13 | 図書館で勉強 | としょかんでべんきょう | としょかんでぺんきょう | `toshokanndepennkyou` | key_far | b→p   |
| far14 | 意見を聞かせて | いけんをきかせて | いけんのきかせて | `ikennnokikasete` | key_far | w→n（を→の）   |

## Insertion / Involvement（15 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| ins01 | 楽しみにしてるね | たのしみにしてるね | たのしみおにしてるね | `tanoshimionishiterune` | insertion | i の後に隣の o   |
| ins02 | 申し訳ございません | もうしわけございません | もうしわjけございません | `moushiwajkegozaimasenn` | insertion | k の前に隣の j  正解が学習にある |
| ins03 | お手数ですが | おてすうですが | おてすうdせすが | `otesuudsesuga` | insertion | d の後に隣の s  正解が学習にある |
| ins04 | 誕生日おめでとう | たんじょうびおめでとう | たんじょうびおめでrとう | `tannjoubiomedertou` | insertion | e の後に隣の r   |
| ins05 | お願いします | おねがいします | おねgはいします | `oneghaishimasu` | insertion | g の後に隣の h   |
| ins06 | 楽しい時間 | たのしいじかん | たのしいjきかん | `tanoshiijkikann` | insertion | j の後に隣の k   |
| ins07 | 行ってきます | いってきます | いうってきます | `iuttekimasu` | insertion | i の後に隣の u  正解が学習にある |
| ins08 | すみません | すみません | すみんません | `suminmasenn` | insertion | m の後に隣の n（ん が増える）  正解が学習にある |
| ins09 | 教えてください | おしえてください | おしえwてください | `oshiewtekudasai` | insertion | e の後に隣の w   |
| ins10 | もう一度 | もういちど | もういちdぽ | `mouichidpo` | insertion | o の前に隣の p  正解が学習にある |
| ins11 | 今週中に | こんしゅうちゅうに | こんしゅういちゅうに | `konnshuuichuuni` | insertion | u の後に隣の i  正解が学習にある |
| ins12 | お待たせしました | おまたせしました | おまたsでしました | `omatasdeshimashita` | insertion | s の後に隣の d  正解が学習にある |
| ins13 | 確認します | かくにんします | かsくにんします | `kaskuninnshimasu` | insertion | a の後に隣の s  正解が学習にある |
| ins14 | ありがとう | ありがとう | ありgふぁとう | `arigfatou` | insertion | g の後に隣の f   |
| ins15 | 行こうよ | いこうよ | いこぷよ | `ikopuyo` | insertion | o の後に隣の p  正解が学習にある |

## Insertion / Other（5 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| ins16 | 週末の予定 | しゅうまつのよてい | しゅうまつのきょてい | `shuumatsunokyotei` | insertion | 離れた k   |
| ins17 | 忙しい | いそがしい | いそがmしい | `isogamshii` | insertion | 離れた m   |
| ins18 | 映画を見た | えいがをみた | えいがをみwた | `eigawomiwta` | insertion | 離れた w  正解が学習にある |
| ins19 | 返事をください | へんじをください | へんじをbください | `hennjiwobkudasai` | insertion | 離れた b   |
| ins20 | ご飯を作る | ごはんをつくる | ごはんをつくzる | `gohannwotsukuzru` | insertion | 離れた z   |

## Insertion / Repetition（14 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| rep01 | 駅に着いたら連絡して | えきについたられんらくして | えきについたられんんらくして | `ekinitsuitararennnrakushite` | repeated_key | n を 3 回（論文 F: ん の nnn）   |
| rep02 | 全然大丈夫 | ぜんぜんだいじょうぶ | ぜんんぜんだいじょうぶ | `zennnzenndaijoubu` | repeated_key | n を 3 回   |
| rep03 | こんにちは、はじめまして | こんにちは、はじめまして | こんんいちは、はじめまして | `konnnnichiha,hajimemashite` | repeated_key | n を 4 回（論文 Table 1 の Repetition）   |
| rep04 | もう寝るね | もうねるね | もうねっるね | `mounerrune` | repeated_key | r を重ねて っ   |
| rep05 | お腹すいた | おなかすいた | おなっかすいた | `onakkasuita` | repeated_key | k を重ねて っ   |
| rep06 | 久しぶりに会えて | ひさしぶりにあえて | ひさしぶりにああえて | `hisashiburiniaaete` | repeated_key | a を重ねる   |
| rep07 | 今後の課題として | こんごのかだいとして | こんごのかっだいとして | `konngonokaddaitoshite` | repeated_key | d を重ねて っ  正解が学習にある |
| rep08 | 政府は発表した | せいふははっぴょうした | せいふっははっぴょうした | `seifuhhahappyoushita` | repeated_key | h を重ねて っ   |
| rep09 | 今晩は | こんばんは | こんっばんは | `konnbbannha` | repeated_key | b を重ねて っ  正解が学習にある |
| rep10 | 待ってください | まってください | まっってください | `matttekudasai` | excessive_double_consonant | 促音の t を 3 回  正解が学習にある |
| rep11 | 来週の火曜日 | らいしゅうのかようび | らいしゅうのっかようび | `raishuunokkayoubi` | repeated_key | k を重ねて っ   |
| rep12 | 締め切りに間に合うように頑張ります | しめきりにまにあうようにがんばります | しめきりにっまにあうようにがんばります | `shimekirinimmaniauyounigannbarimasu` | repeated_key | m を重ねて っ   |
| rep13 | ちょっと考えさせて | ちょっとかんがえさせて | ちょっとかんがええさせて | `chottokanngaeesasete` | repeated_key | e を重ねる  正解が学習にある |
| rep14 | 本当にありがとう | ほんとうにありがとう | ほんとうにあっりがとう | `honntouniarrigatou` | repeated_key | r を重ねて っ  正解が学習にある |

## Exchange（8 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| exc01 | お疲れさまです | おつかれさまです | おつかれさmだえす | `otsukaresamdaesu` | transposition | a と d  正解が学習にある |
| exc02 | ご確認をお願いいたします | ごかくにんをおねがいいたします | ごかくにんをおねがいいたhします | `gokakuninnwoonegaiitahsimasu` | transposition | s と h   |
| exc03 | 本日中に | ほんじつちゅうに | ほんじつhくうに | `honnjitsuhcuuni` | transposition | c と h  正解が学習にある |
| exc04 | 議事録を共有します | ぎじろくをきょうゆうします | ぎじろくをきゅおゆうします | `gijirokuwokyuoyuushimasu` | transposition | o と u   |
| exc05 | 雨が降ってきた | あめがふってきた | あめがふてtきた | `amegafutetkita` | transposition | t と e   |
| exc06 | 猫がかわいい | ねこがかわいい | ねこがkわあいい | `nekogakwaaii` | transposition | a と w   |
| exc07 | 東京都内の | とうきょうとないの | とうきゅおとないの | `toukyuotonaino` | transposition | o と u  正解が学習にある |
| exc08 | また連絡するね | またれんらくするね | またれんらkすうるね | `matarennraksuurune` | transposition | u と s   |

## 2 つの誤り（4 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| mul01 | 明日は雨らしい | あしたはあめらしい | あしょたああめらしい | `ashotaaamerashii` | substitution+deletion | i→o と h が抜ける   |
| mul02 | 駅まで迎えに行くよ | えきまでむかえにいくよ | えきmsでむかえのいくよ | `ekimsdemukaenoikuyo` | substitution+substitution | a→s と i→o   |
| mul03 | 資料を確認しました | しりょうをかくにんしました | しりゅおをかきにんしました | `shiryuowokakininnshimashita` | transposition+substitution | o と u の入れ替え と u→i   |
| mul04 | 忘れ物してない | わすれものしてない | わすれののしいぇない | `wasurenonoshiyenai` | substitution+substitution | m→n と t→y   |

## 誤りなし（60 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| cln01 | 了解しました | りょうかいしました | りょうかいしました |  | none |   正解が学習にある |
| cln02 | ちょっと待ってね | ちょっとまってね | ちょっとまってね |  | none | 促音が 2 つ  正解が学習にある |
| cln03 | すごーい | すごーい | すごーい |  | none | 長音   |
| cln04 | やっぱりそうだよね | やっぱりそうだよね | やっぱりそうだよね |  | none |    |
| cln05 | ぐるぐる回る | ぐるぐるまわる | ぐるぐるまわる |  | none | 正しい繰り返し  正解が学習にある |
| cln06 | いよいよ明日 | いよいよあした | いよいよあした |  | none | 正しい繰り返し   |
| cln07 | のどがからから | のどがからから | のどがからから |  | none | 正しい繰り返し（からから は二重打ちに見える）   |
| cln08 | ここに置いといて | ここにおいといて | ここにおいといて |  | none | 正しい繰り返し   |
| cln09 | なんでやねん | なんでやねん | なんでやねん |  | none | 方言  正解が学習にある |
| cln10 | そうやけんね | そうやけんね | そうやけんね |  | none | 方言   |
| cln11 | めっちゃええやん | めっちゃええやん | めっちゃええやん |  | none | 方言・母音の連続   |
| cln12 | まじでやばい | まじでやばい | まじでやばい |  | none | くだけた言い方   |
| cln13 | ぴえんすぎる | ぴえんすぎる | ぴえんすぎる |  | none | 新語   |
| cln14 | ファイルを開く | ふぁいるをひらく | ふぁいるをひらく |  | none | 小さい仮名の外来語   |
| cln15 | ウェブサイト | うぇぶさいと | うぇぶさいと |  | none | 小さい仮名の外来語  正解が学習にある |
| cln16 | ディスプレイが割れた | でぃすぷれいがわれた | でぃすぷれいがわれた |  | none | 小さい仮名の外来語   |
| cln17 | キャンセル待ち | きゃんせるまち | きゃんせるまち |  | none | 外来語  正解が学習にある |
| cln18 | ぽかぽか陽気 | ぽかぽかようき | ぽかぽかようき |  | none | 擬態語   |
| cln19 | もふもふの猫 | もふもふのねこ | もふもふのねこ |  | none | 擬態語   |
| cln20 | えっ、ほんとに | えっ、ほんとに | えっ、ほんとに |  | none | 促音で終わる感動詞   |
| cln21 | んー、どうしよう | んー、どうしよう | んー、どうしよう |  | none | ん で始まる   |
| cln22 | おっけー | おっけー | おっけー |  | none | 促音と長音  正解が学習にある |
| cln23 | はいはい、わかった | はいはい、わかった | はいはい、わかった |  | none | 正しい繰り返し   |
| cln24 | ありがとうね | ありがとうね | ありがとうね |  | none |   正解が学習にある |
| cln25 | お疲れさまでした | おつかれさまでした | おつかれさまでした |  | none |   正解が学習にある |
| cln26 | 佐渡に行きたい | さどにいきたい | さどにいきたい |  | none | さど は さいど の typo に見える（CLAUDE.md の例）   |
| cln27 | 再度確認します | さいどかくにんします | さいどかくにんします |  | none |    |
| cln28 | きっと大丈夫 | きっとだいじょうぶ | きっとだいじょうぶ |  | none |    |
| cln29 | ちゃっかりしてる | ちゃっかりしてる | ちゃっかりしてる |  | none | 拗音と促音   |
| cln30 | しっかり者 | しっかりもの | しっかりもの |  | none |   正解が学習にある |
| cln31 | 八王子駅 | はちおうじえき | はちおうじえき |  | none | 地名  正解が学習にある |
| cln32 | さいたま市 | さいたまし | さいたまし |  | none | 地名  正解が学習にある |
| cln33 | 宇都宮の餃子 | うつのみやのぎょうざ | うつのみやのぎょうざ |  | none | 地名   |
| cln34 | 三時半ごろ | さんじはんごろ | さんじはんごろ |  | none | 数の読み   |
| cln35 | 百二十円 | ひゃくにじゅうえん | ひゃくにじゅうえん |  | none | 数の読み   |
| cln36 | 午前中に | ごぜんちゅうに | ごぜんちゅうに |  | none |    |
| cln37 | 本日はお日柄もよく | ほんじつはおひがらもよく | ほんじつはおひがらもよく |  | none | 改まった言い方   |
| cln38 | ご査収ください | ごさしゅうください | ごさしゅうください |  | none | まれなビジネス語   |
| cln39 | 取り急ぎご連絡まで | とりいそぎごれんらくまで | とりいそぎごれんらくまで |  | none |    |
| cln40 | ご無沙汰しております | ごぶさたしております | ごぶさたしております |  | none |    |
| cln41 | 検討させていただきます | けんとうさせていただきます | けんとうさせていただきます |  | none |    |
| cln42 | おかげさまで | おかげさまで | おかげさまで |  | none |    |
| cln43 | 先行研究を概観する | せんこうけんきゅうをがいかんする | せんこうけんきゅうをがいかんする |  | none |    |
| cln44 | 有意差は認められなかった | ゆういさはみとめられなかった | ゆういさはみとめられなかった |  | none |   正解が学習にある |
| cln45 | 仮説を支持する | かせつをしじする | かせつをしじする |  | none |   正解が学習にある |
| cln46 | 急きょ中止になった | きゅうきょちゅうしになった | きゅうきょちゅうしになった |  | none | きゅうきょ は きょうきょ 等の typo に見える  正解が学習にある |
| cln47 | しょっちゅうある | しょっちゅうある | しょっちゅうある |  | none | 拗音と促音   |
| cln48 | ぎりぎり間に合った | ぎりぎりまにあった | ぎりぎりまにあった |  | none | 正しい繰り返し   |
| cln49 | ささっと済ませる | ささっとすませる | ささっとすませる |  | none | 正しい繰り返し   |
| cln50 | 九日の予定 | ここのかのよてい | ここのかのよてい |  | none | ここ の繰り返し   |
| cln51 | ええと、 | ええと、 | ええと、 |  | none | 母音の連続  正解が学習にある |
| cln52 | あのー、すみません | あのー、すみません | あのー、すみません |  | none |    |
| cln53 | いいえ、違います | いいえ、ちがいます | いいえ、ちがいます |  | none | 母音の連続   |
| cln54 | 大きい犬 | おおきいいぬ | おおきいいぬ |  | none | い が 3 つ続く   |
| cln55 | こういうこと | こういうこと | こういうこと |  | none |   正解が学習にある |
| cln56 | 初々しい | ういういしい | ういういしい |  | none | 母音の連続  正解が学習にある |
| cln57 | いっぱいいっぱい | いっぱいいっぱい | いっぱいいっぱい |  | none | 正しい繰り返し  正解が学習にある |
| cln58 | そうそう、それそれ | そうそう、それそれ | そうそう、それそれ |  | none | 正しい繰り返し   |
| cln59 | お世話になって | おせわになって | おせわになって |  | none | 打ちかけ（この後に おります が続く）  正解が学習にある |
| cln60 | 確認させて | かくにんさせて | かくにんさせて |  | none | 打ちかけ（この後に いただきます が続く）  正解が学習にある |
