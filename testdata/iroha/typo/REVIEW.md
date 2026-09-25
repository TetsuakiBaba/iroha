# typo ベンチマーク 確認用一覧

`bench.tsv` から `make_bench.py` が作る（手で直さない）。直すときは bench.tsv を編集して作り直す。

## Removal（21 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| sub01 | 今日は楽しかった、また五人で集まろう | きょうはたのしかった、またごにんであつまろう | きょはたのしかった、またごにんであつまろう | `kyohatanoshikatta,matagoninndeatsumarou` | deletion | kyou の u が抜ける（ょう が ょ になる）   |
| del01 | ありがとうございます、先日の見積書を拝見しました | ありがとうございます、せんじつのみつもりしょをはいけんしました | ありがとうgざいます、せんじつのみつもりしょをはいけんしました | `arigatougzaimasu,sennjitsunomitsumorishowohaikennshimashita` | deletion | o が抜ける   |
| del02 | 写真送るね | しゃしんおくるね | しゃしのくるね | `shashinokurune` | deletion | ん の n が 1 つ（n+o が の になる）   |
| del03 | 今度ご飯行こう | こんどごはんいこう | こんどごはにこう | `konndogohanikou` | deletion | ん の n が 1 つ（n+i が に）   |
| del04 | 締め切りは金曜です | しめきりはきんようです | しめきりはきにょうです | `shimekirihakinyoudesu` | deletion | ん の n が 1 つ（n+yo が にょ）   |
| del05 | 実験の結果をグラフにまとめる | じっけんのけっかをぐらふにまとめる | じっけんおけっかをぐらふにまとめる | `jikkennokekkawogurafunimatomeru` | deletion | nn の後の n が抜ける（ん の後の の が お）   |
| del06 | 駅前のパン屋 | えきまえのぱんや | えきまえのぱにゃ | `ekimaenopanya` | deletion | ん の n が 1 つ（n+ya が にゃ）   |
| del07 | 簡易的な方法 | かんいてきなほうほう | かにいてきなほうほう | `kaniitekinahouhou` | deletion | ん の n が 1 つ（n+i が に）   |
| del08 | 打ち合わせの日程 | うちあわせのにってい | うちあわせのにてい | `uchiawasenonitei` | missing_double_consonant | 促音の t が 1 つ   |
| del09 | 熱があったけど学校に行った | ねつがあったけどがっこうにいった | ねつがあったけどがこうにいった | `netsugaattakedogakouniitta` | missing_double_consonant | 促音の k が 1 つ   |
| del10 | やっぱりそうか、そんな気がしてた | やっぱりそうか、そんなきがしてた | やぱりそうか、そんなきがしてた | `yaparisouka,sonnnakigashiteta` | missing_double_consonant | 促音の p が 1 つ   |
| del11 | 切符を買った | きっぷをかった | きぷをかった | `kipuwokatta` | missing_double_consonant | 促音の p が 1 つ   |
| del12 | 一緒に行こう、駅で待ってるね | いっしょにいこう、えきでまってるね | いしょにいこう、えきでまってるね | `ishoniikou,ekidematterune` | missing_double_consonant | 促音の s が 1 つ   |
| del13 | 荷物を預けて空港まで | にもつをあずけてくうこうまで | にもつをあずけてくこうまで | `nimotsuwoazuketekukoumade` | deletion | u が抜ける   |
| del14 | 話題の新作を映画館で見た | わだいのしんさくをえいがかんでみた | わだいのしんさくをえいあかんでみた | `wadainoshinnsakuwoeiakanndemita` | deletion | g が抜ける   |
| del15 | 今日の予定 | きょうのよてい | きょうのおてい | `kyounootei` | deletion | y が抜ける   |
| del16 | 旅行の準備 | りょこうのじゅんび | ろこうのじゅんび | `rokounojunnbi` | deletion | y が抜ける   |
| del17 | 聞いてください、会議の録音を | きいてください、かいぎのろくおんを | きいてくあさい、かいぎのろくおんを | `kiitekuasai,kaiginorokuonnwo` | deletion | d が抜ける   |
| del18 | 分かりました、会場の予約を変更します | わかりました、かいじょうのよやくをへんこうします | わかrました、かいじょうのよやくをへんこうします | `wakarmashita,kaijounoyoyakuwohennkoushimasu` | deletion | ri の i が抜ける（r だと わかい になり 若い と区別できない）   |
| del19 | 考えておきます、来月の出張の件 | かんがえておきます、らいげつのしゅっちょうのけん | かんあえておきます、らいげつのしゅっちょうのけん | `kannaeteokimasu,raigetsunoshucchounokenn` | deletion | g が抜ける   |
| del20 | 仮説を検証する | かせつをけんしょうする | かせtsをけんしょうする | `kasetswokennshousuru` | deletion | tsu の u が抜ける   |

## Replacement（隣接キー）（59 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| sub02 | 改札の前でちょっと待ってて | かいさつのまえでちょっとまってて | かいさつのまえでちっとまってて | `kaisatsunomaedechittomattete` | substitution | o→i   |
| sub03 | 今から行くね、改札の横で待ってて | いまからいくね、かいさつのよこでまってて | いまからいきね、かいさつのよこでまってて | `imakaraikine,kaisatsunoyokodemattete` | substitution | u→i（論文の I/U）   |
| sub04 | 傘なら二本あるから大丈夫だよ | かさならにほんあるからだいじょうぶだよ | かさならにほんあるからだいじうぶだよ | `kasanaranihonnarukaradaijiubudayo` | substitution | o→i   |
| sub05 | 何時に集合する | なんじにしゅうごうする | なんじにしゅうごうづる | `nannjinishuugouduru` | substitution | s→d   |
| sub06 | 来月の最初の週末空いてる | らいげつのさいしょのしゅうまつあいてる | らいげつのさいしょのしゅうまrすあいてる | `raigetsunosaishonoshuumarsuaiteru` | substitution | t→r   |
| sub07 | 昨日の夜更かしでめっちゃ眠い | きのうのよふかしでめっちゃねむい | きのうのよふかしでめっちゃねぬい | `kinounoyofukashidemecchanenui` | substitution | m→n   |
| sub08 | 喉が痛いから風邪ひいたかも | のどがいたいからかぜひいたかも | のどがいたいからかぇひいたかも | `nodogaitaikarakaxehiitakamo` | substitution | z→x（xe が小さい ぇ になる）   |
| sub09 | 返信遅れてごめん | へんしんおくれてごめん | へんしんおくててごめん | `hennshinnokutetegomenn` | substitution | r→t   |
| sub10 | お世話になっております、佐藤です | おせわになっております、さとうです | おsrわになっております、さとうです | `osrwaninatteorimasu,satoudesu` | substitution | e→r   |
| sub11 | 資料を添付しました | しりょうをてんぷしました | しりょうをてんぷしmっした | `shiryouwotennpushimsshita` | substitution | a→s（ss が促音になる）   |
| sub12 | よろしくお願いします、打ち合わせの議事録は明日送ります | よろしくおねがいします、うちあわせのぎじろくはあしたおくります | よろしくおねgしします、うちあわせのぎじろくはあしたおくります | `yoroshikuonegsishimasu,uchiawasenogijirokuhaashitaokurimasu` | substitution | a→s   |
| sub13 | 承知いたしました、さっそく手配します | しょうちいたしました、さっそくてはいします | しょうちいたしまsぎた、さっそくてはいします | `shouchiitashimasgita,sassokutehaishimasu` | substitution | h→g   |
| sub14 | 恐れ入りますが折り返しお電話ください | おそれいりますがおりかえしおでんわください | おそえいりますがおりかえしおでんわください | `osoeirimasugaorikaeshiodennwakudasai` | substitution | r→e   |
| sub15 | 至急ご対応ください | しきゅうごたいおうください | しkつうごたいおうください | `shiktuugotaioukudasai` | substitution | y→t   |
| sub16 | 見積書を送付します | みつもりしょをそうふします | みつもりしょをそうぐします | `mitsumorishowosougushimasu` | substitution | f→g   |
| sub17 | 折り返しご連絡します | おりかえしごれんらくします | おりぁえしごれんらくします | `orilaeshigorennrakushimasu` | substitution | k→l（la が小さい ぁ になる）   |
| sub18 | 新しいカフェに行ってきた | あたらしいかふぇにいってきた | あたらしゅいかふぇにいってきた | `atarashuikafeniittekita` | substitution | i→u   |
| sub19 | 電車が遅れてる | でんしゃがおくれてる | でんshsがおくれてる | `dennshsgaokureteru` | substitution | a→s   |
| sub20 | 寝坊したけど朝ごはん食べた | ねぼうしたけどあさごはんたべた | ねぼうしたけどあさごはんたねた | `neboushitakedoasagohanntaneta` | substitution | b→n   |
| sub21 | 本研究では | ほんけんきゅうでは | ほんけんくううでは | `honnkennkuuudeha` | substitution | y→u   |
| sub22 | 先行研究によればこの手法は有効だ | せんこうけんきゅうによればこのしゅほうはゆうこうだ | せんこうけんきゅうによええばこのしゅほうはゆうこうだ | `sennkoukennkyuuniyoeebakonoshuhouhayuukouda` | substitution | r→e   |
| sub23 | 有意な差が見られた | ゆういなさがみられた | ゆういなさがもられた | `yuuinasagamorareta` | substitution | i→o（もられた は語としてある）   |
| sub24 | 統計的に有意な差が見られた | とうけいてきにゆういなさがみられた | とうけいいぇきにゆういなさがみられた | `toukeiyekiniyuuinasagamirareta` | substitution | t→y   |
| sub25 | 予備実験での被験者の数 | よびじっけんでのひけんしゃのかず | よびじっけんでのひけんしゃのかあう | `yobijikkenndenohikennshanokaau` | substitution | z→a   |
| sub26 | 図に示すように | ずにしめすように | ずにしねすように | `zunishinesuyouni` | substitution | m→n   |
| sub27 | 社長に報告 | しゃちょうにほうこく | しゃちうにほうこく | `shachiunihoukoku` | substitution | o→i   |
| sub28 | 急いでください、締め切りは今日の夕方です | いそいでください、しめきりはきょうのゆうがたです | いそいできださい、しめきりはきょうのゆうがたです | `isoidekidasai,shimekirihakyounoyuugatadesu` | substitution | u→i   |
| sub29 | 普通に考えてあの値段は高すぎる | ふつうにかんがえてあのねだんはたかすぎる | ふぃつうにかんがえてあのねだんはたかすぎる | `fitsuunikanngaeteanonedannhatakasugiru` | substitution | u→i（fi が ふぃ になる）   |
| sub30 | 思います、この案で進めてよいと | おもいます、このあんですすめてよいと | おみいます、このあんですすめてよいと | `omiimasu,konoanndesusumeteyoito` | substitution | o→i   |
| sub31 | 仕事終わった、駅前のカフェにいるね | しごとおわった、えきまえのかふぇにいるね | しぎとおわった、えきまえのかふぇにいるね | `shigitoowatta,ekimaenokafeniirune` | substitution | o→i   |
| sub32 | どこにいるの、もう着いたよ | どこにいるの、もうついたよ | ぢこにいるの、もうついたよ | `dikoniiruno,moutsuitayo` | substitution | o→i（di が ぢ になる）   |
| sub33 | 牛乳が少し足りないかも | ぎゅうにゅうがすこしたりないかも | ぎゅうにゅうがすこしtsりないかも | `gyuunyuugasukoshitsrinaikamo` | substitution | a→s   |
| sub34 | 祖母に久しぶりに手紙を書いた | そぼにひさしぶりにてがみをかいた | そぼにひさしぶりにtrがみをかいた | `sobonihisashiburinitrgamiwokaita` | substitution | e→r   |
| sub35 | 電波が悪くて声が聞こえない | でんぱがわるくてこえがきこえない | でんぱがわるくてこえがきぉえない | `dennpagawarukutekoegakiloenai` | substitution | k→l（lo が ぉ になる）   |
| sub36 | 地下鉄で三駅だけ行く | ちかてつでさんえきだけいく | ちじゃてつでさんえきだけいく | `chijatetsudesannekidakeiku` | substitution | k→j   |
| sub37 | 始めます、第三回の定例会議を | はじめます、だいさんかいのていれいかいぎを | がじめます、だいさんかいのていれいかいぎを | `gajimemasu,daisannkainoteireikaigiwo` | substitution | h→g   |
| sub38 | 冷蔵庫に何もない | れいぞうこになにもない | れいぞうこになにのない | `reizoukoninaninonai` | substitution | m→n   |
| sub39 | 地域の防災への取り組み | ちいきのぼうさいへのとりくみ | ちいきのぼうさいへのとちくみ | `chiikinobousaihenototikumi` | substitution | r→t（論文の R/T）   |
| sub40 | 無事に家へ帰りました | ぶじにいえへかえりました | ぶじにいえへかえちました | `bujiniiehekaetimashita` | substitution | r→t   |
| sub41 | 約束の時間 | やくそくのじかん | たくそくのじかん | `takusokunojikann` | substitution | y→t   |
| sub42 | 私は行かない | わたしはいかない | くぁたしはいかない | `qatashihaikanai` | substitution | w→q（qa が くぁ になる）   |
| sub43 | 卒論のために統計を勉強中です | そつろんのためにとうけいをべんきょうちゅうです | そつろんのためにとうけいをねんきょうちゅうです | `sotsuronnnotamenitoukeiwonennkyouchuudesu` | substitution | b→n   |
| sub44 | 新作の発売を期待してる | しんさくのはつばいをきたいしてる | しんさくのはつばいをk8たいしてる | `shinnsakunohatsubaiwok8taishiteru` | substitution | i→8（数字の段）   |
| sub45 | 待ち合わせしよう | まちあわせしよう | まちあわswしよう | `machiawaswshiyou` | substitution | e→w   |
| sub46 | 天気予報では午後から雪 | てんきよほうではごごからゆき | てんきよじょうではごごからゆき | `tennkiyojoudehagogokarayuki` | substitution | h→j   |
| sub47 | 駅前で待ち合わせしよう | えきまえでまちあわせしよう | wきまえでまちあわせしよう | `wkimaedemachiawaseshiyou` | substitution | e→w（先頭）   |
| sub48 | 来週の旅行、楽しみにしてる | らいしゅうのりょこう、たのしみにしてる | らいしゅうのりょこう、たのしみにしてり | `raishuunoryokou,tanoshiminishiteri` | substitution | u→i（末尾）   |
| sub49 | お疲れさま、先に失礼します | おつかれさま、さきにしつれいします | おつかてさま、さきにしつれいします | `otsukatesama,sakinishitsureishimasu` | substitution | r→t   |
| sub50 | ごめんね、昨日の電話に出られなくて | ごめんね、きのうのでんわにでられなくて | ごめんへ、きのうのでんわにでられなくて | `gomennhe,kinounodennwaniderarenakute` | substitution | n→h   |
| sub51 | 会議の資料 | かいぎのしりょう | かいぎのsじりょう | `kaiginosjiryou` | substitution | h→j   |
| sub52 | 週末は弟と映画を観に行く | しゅうまつはおとうととえいがをみにいく | しゅうまつはおとうととえいがをみにいじゅ | `shuumatsuhaotoutotoeigawominiiju` | substitution | k→j   |
| sub53 | 引っ越しの準備ができた | ひっこしのじゅんびができた | ひっこしのじゅんびができら | `hikkoshinojunnbigadekira` | substitution | t→r   |
| sub54 | 明日は休み | あしたはやすみ | あしたはやすに | `ashitahayasuni` | substitution | m→n   |
| sub55 | 冷蔵庫に入れて | れいぞうこにいれて | れいぞうぉにいれて | `reizouloniirete` | substitution | k→l   |
| sub56 | 帰りに洗剤を買ってきてほしい | かえりにせんざいをかってきてほしい | かえりにせんざいをかってきてじょしい | `kaerinisennzaiwokattekitejoshii` | substitution | h→j   |
| sub57 | ご都合いかがですか | ごつごういかがですか | ごつごういぁがですか | `gotsugouilagadesuka` | substitution | k→l   |
| sub58 | 年末年始の営業時間について | ねんまつねんしのえいぎょうじかんについて | ねんなつねんしのえいぎょうじかんについて | `nennnatsunennshinoeigyoujikannnitsuite` | substitution | m→n   |
| sub59 | 電話してもいい | でんわしてもいい | でんえあしてもいい | `denneashitemoii` | substitution | w→e   |
| sub60 | 帰り道は暗いから気をつけてね | かえりみちはくらいからきをつけてね | かえりみちはくらいからきをつけいぇね | `kaerimichihakuraikarakiwotsukeyene` | substitution | t→y   |

## Replacement（離れたキー）（14 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| far01 | 了解です、明日は八時に迎えに行く | りょうかいです、あしたははちじにむかえにいく | りゅうかいです、あしたははちじにむかえにいく | `ryuukaidesu,ashitahahachijinimukaeniiku` | key_far | o→u（論文 E: ょ/ゅ の取り違え）   |
| far02 | 気をつけて帰ってね | きをつけてかえってね | きのつけてかえってね | `kinotsuketekaettene` | key_far | w→n（を→の。JWTD で最多）   |
| far03 | 暖房が壊れて部屋が寒い | だんぼうがこわれてへやがさむい | だんぼうがこわれてへやかさむい | `dannbougakowareteheyakasamui` | key_far | g→k（が→か）   |
| far04 | 天気がいいから散歩しよう | てんきがいいからさんぽしよう | てんきがいいからさんぼしよう | `tennkigaiikarasannboshiyou` | key_far | p→b   |
| far05 | 収集したデータの分析を行った | しゅうしゅうしたでーたのぶんせきをおこなった | しゅうしゅうしたでーたのぶんせきのおこなった | `shuushuushitade-tanobunnsekinookonatta` | key_far | w→n（を→の）   |
| far06 | 会社に行く | かいしゃにいく | かいしょにいく | `kaishoniiku` | key_far | a→o（論文 H: SHA/SHO）   |
| far07 | 卒業式の日に話したかった | そつぎょうしきのひにはなしたかった | そつぎょうしきのひにほなしたかった | `sotsugyoushikinohinihonashitakatta` | key_far | a→o（論文 H: A/O）   |
| far08 | 新しい資料は準備中 | あたらしいしりょうはじゅんびちゅう | あたらしいしりょうはじょんびちゅう | `atarashiishiryouhajonnbichuu` | key_far | u→o（論文 E: U/O）   |
| far09 | スマホで地図が表示されない | すまほでちずがひょうじされない | すまほでちずがひゅうじされない | `sumahodechizugahyuujisarenai` | key_far | o→u   |
| far10 | 少しだけ待っててもらえると助かる | すこしだけまっててもらえるとたすかる | すそしだけまっててもらえるとたすかる | `susoshidakemattetemoraerutotasukaru` | key_far | k→s（論文 A: K/S）   |
| far11 | 今夜のおかず、何にする | こんやのおかず、なににする | こんやのおかず、ないいにする | `konnyanookazu,naiinisuru` | key_far | n→i（論文 A: N/I）   |
| far12 | 詳しいことは後で説明します | くわしいことはあとでせつめいします | くわしいことはあとてせつめいします | `kuwashiikotohaatotesetsumeishimasu` | key_far | d→t   |
| far13 | 図書館で勉強 | としょかんでべんきょう | としょかんでぺんきょう | `toshokanndepennkyou` | key_far | b→p   |
| far14 | 意見を聞かせて | いけんをきかせて | いけんのきかせて | `ikennnokikasete` | key_far | w→n（を→の）   |

## Insertion / Involvement（15 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| ins01 | 週末の花火大会、楽しみにしてるね | しゅうまつのはなびたいかい、たのしみにしてるね | しゅうまつのはなびたいかい、たのしみおにしてるね | `shuumatsunohanabitaikai,tanoshimionishiterune` | insertion | i の後に隣の o   |
| ins02 | 申し訳ございません、在庫を切らしております | もうしわけございません、ざいこをきらしております | もうしわjけございません、ざいこをきらしております | `moushiwajkegozaimasenn,zaikowokirashiteorimasu` | insertion | k の前に隣の j   |
| ins03 | お手数ですが同封の用紙をご返送ください | おてすうですがどうふうのようしをごへんそうください | おてすうdせすがどうふうのようしをごへんそうください | `otesuudsesugadoufuunoyoushiwogohennsoukudasai` | insertion | d の後に隣の s   |
| ins04 | 妹へ、誕生日おめでとう | いもうとへ、たんじょうびおめでとう | いもうとへ、たんじょうびおめでrとう | `imoutohe,tannjoubiomedertou` | insertion | e の後に隣の r   |
| ins05 | お願いします、請求書の再発行を | おねがいします、せいきゅうしょのさいはっこうを | おねgはいします、せいきゅうしょのさいはっこうを | `oneghaishimasu,seikyuushonosaihakkouwo` | insertion | g の後に隣の h   |
| ins06 | 楽しい時間 | たのしいじかん | たのしいjきかん | `tanoshiijkikann` | insertion | j の後に隣の k   |
| ins07 | 行ってきます、夕飯までには戻るね | いってきます、ゆうはんまでにはもどるね | いうってきます、ゆうはんまでにはもどるね | `iuttekimasu,yuuhannmadenihamodorune` | insertion | i の後に隣の u   |
| ins08 | すみません、傘を貸してもらえますか | すみません、かさをかしてもらえますか | すみんません、かさをかしてもらえますか | `suminmasenn,kasawokashitemoraemasuka` | insertion | m の後に隣の n（ん が増える）   |
| ins09 | 返品の手順を教えてください | へんぴんのてじゅんをおしえてください | へんぴんのてじゅんをおしえwてください | `hennpinnnotejunnwooshiewtekudasai` | insertion | e の後に隣の w   |
| ins10 | もう一度だけ説明してもらえる | もういちどだけせつめいしてもらえる | もういちdぽだけせつめいしてもらえる | `mouichidpodakesetsumeishitemoraeru` | insertion | o の前に隣の p   |
| ins11 | 今週中に見積もりを送ります | こんしゅうちゅうにみつもりをおくります | こんしゅういちゅうにみつもりをおくります | `konnshuuichuunimitsumoriwookurimasu` | insertion | u の後に隣の i   |
| ins12 | お待たせしました、会議室へご案内します | おまたせしました、かいぎしつへごあんないします | おまたsでしました、かいぎしつへごあんないします | `omatasdeshimashita,kaigishitsuhegoannnaishimasu` | insertion | s の後に隣の d   |
| ins13 | 確認しますので少々お時間ください | かくにんしますのでしょうしょうおじかんください | かsくにんしますのでしょうしょうおじかんください | `kaskuninnshimasunodeshoushouojikannkudasai` | insertion | a の後に隣の s   |
| ins14 | ありがとう、荷物を受け取ってくれて | ありがとう、にもつをうけとってくれて | ありgふぁとう、にもつをうけとってくれて | `arigfatou,nimotsuwouketottekurete` | insertion | g の後に隣の f   |
| ins15 | 今度の連休に温泉へ行こうよ | こんどのれんきゅうにおんせんへいこうよ | こんどのれんきゅうにおんせんへいこぷよ | `konndonorennkyuunionnsennheikopuyo` | insertion | o の後に隣の p   |

## Insertion / Other（5 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| ins16 | 週末の予定 | しゅうまつのよてい | しゅうまつのきょてい | `shuumatsunokyotei` | insertion | 離れた k   |
| ins17 | 年度末でみんな忙しい | ねんどまつでみんないそがしい | ねんどまつでみんないそがmしい | `nenndomatsudeminnnaisogamshii` | insertion | 離れた m   |
| ins18 | 夜中に怖い映画を見た | よなかにこわいえいがをみた | よなかにこわいえいがをみwた | `yonakanikowaieigawomiwta` | insertion | 離れた w   |
| ins19 | 返事をください | へんじをください | へんじをbください | `hennjiwobkudasai` | insertion | 離れた b   |
| ins20 | ご飯を作る | ごはんをつくる | ごはんをつくzる | `gohannwotsukuzru` | insertion | 離れた z   |

## Insertion / Repetition（14 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| rep01 | 駅に着いたら連絡して | えきについたられんらくして | えきについたられんんらくして | `ekinitsuitararennnrakushite` | repeated_key | n を 3 回（論文 F: ん の nnn）   |
| rep02 | 全然大丈夫、気にしないで次いこう | ぜんぜんだいじょうぶ、きにしないでつぎいこう | ぜんんぜんだいじょうぶ、きにしないでつぎいこう | `zennnzenndaijoubu,kinishinaidetsugiikou` | repeated_key | n を 3 回   |
| rep03 | こんにちは、はじめまして、隣に越してきた者です | こんにちは、はじめまして、となりにこしてきたものです | こんんいちは、はじめまして、となりにこしてきたものです | `konnnnichiha,hajimemashite,tonarinikoshitekitamonodesu` | repeated_key | n を 4 回（論文 Table 1 の Repetition）   |
| rep04 | 明日早いからもう寝るね | あしたはやいからもうねるね | あしたはやいからもうねっるね | `ashitahayaikaramounerrune` | repeated_key | r を重ねて っ   |
| rep05 | 部活帰りでお腹すいた | ぶかつがえりでおなかすいた | ぶかつがえりでおなっかすいた | `bukatsugaerideonakkasuita` | repeated_key | k を重ねて っ   |
| rep06 | 久しぶりに会えて | ひさしぶりにあえて | ひさしぶりにああえて | `hisashiburiniaaete` | repeated_key | a を重ねる   |
| rep07 | 今後の課題としてデータの蓄積が挙げられる | こんごのかだいとしてでーたのちくせきがあげられる | こんごのかっだいとしてでーたのちくせきがあげられる | `konngonokaddaitoshitede-tanochikusekigaagerareru` | repeated_key | d を重ねて っ   |
| rep08 | 政府は発表した | せいふははっぴょうした | せいふっははっぴょうした | `seifuhhahappyoushita` | repeated_key | h を重ねて っ   |
| rep09 | 今晩は、今日も一日お疲れ | こんばんは、きょうもいちにちおつかれ | こんっばんは、きょうもいちにちおつかれ | `konnbbannha,kyoumoichinichiotsukare` | repeated_key | b を重ねて っ   |
| rep10 | 待ってください、すぐに担当者が参ります | まってください、すぐにたんとうしゃがまいります | まっってください、すぐにたんとうしゃがまいります | `matttekudasai,sugunitanntoushagamairimasu` | excessive_double_consonant | 促音の t を 3 回   |
| rep11 | 来週の火曜日 | らいしゅうのかようび | らいしゅうのっかようび | `raishuunokkayoubi` | repeated_key | k を重ねて っ   |
| rep12 | 締め切りに間に合うように頑張ります | しめきりにまにあうようにがんばります | しめきりにっまにあうようにがんばります | `shimekirinimmaniauyounigannbarimasu` | repeated_key | m を重ねて っ   |
| rep13 | ちょっと考えさせて、明日には返事するから | ちょっとかんがえさせて、あしたにはへんじするから | ちょっとかんがええさせて、あしたにはへんじするから | `chottokanngaeesasete,ashitanihahennjisurukara` | repeated_key | e を重ねる   |
| rep14 | 本当にありがとう、おかげで助かったよ | ほんとうにありがとう、おかげでたすかったよ | ほんとうにあっりがとう、おかげでたすかったよ | `honntouniarrigatou,okagedetasukattayo` | repeated_key | r を重ねて っ   |

## Exchange（8 件）

| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ  コーパス |
|---|---|---|---|---|---|---|---|
| exc01 | お疲れさまです、資料は共有フォルダにあります | おつかれさまです、しりょうはきょうゆうふぉるだにあります | おつかれさmだえす、しりょうはきょうゆうふぉるだにあります | `otsukaresamdaesu,shiryouhakyouyuuforudaniarimasu` | transposition | a と d   |
| exc02 | ご確認をお願いいたします | ごかくにんをおねがいいたします | ごかくにんをおねがいいたhします | `gokakuninnwoonegaiitahsimasu` | transposition | s と h   |
| exc03 | 本日中に折り返しご連絡いたします | ほんじつちゅうにおりかえしごれんらくいたします | ほんじつhくうにおりかえしごれんらくいたします | `honnjitsuhcuuniorikaeshigorennrakuitashimasu` | transposition | c と h   |
| exc04 | 議事録を共有します | ぎじろくをきょうゆうします | ぎじろくをきゅおゆうします | `gijirokuwokyuoyuushimasu` | transposition | o と u   |
| exc05 | 洗濯物を干したのに雨が降ってきた | せんたくものをほしたのにあめがふってきた | せんたくものをほしたのにあめがふてtきた | `senntakumonowohoshitanoniamegafutetkita` | transposition | t と e   |
| exc06 | 猫がかわいい | ねこがかわいい | ねこがkわあいい | `nekogakwaaii` | transposition | a と w   |
| exc07 | 東京都内の病院の空き状況 | とうきょうとないのびょういんのあきじょうきょう | とうきゅおとないのびょういんのあきじょうきょう | `toukyuotonainobyouinnnoakijoukyou` | transposition | o と u   |
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
| cln01 | 了解しました、明日の十時に伺います | りょうかいしました、あしたのじゅうじにうかがいます | りょうかいしました、あしたのじゅうじにうかがいます |  | none |    |
| cln02 | ちょっと待ってね、今鍵を探してる | ちょっとまってね、いまかぎをさがしてる | ちょっとまってね、いまかぎをさがしてる |  | none | 促音が 2 つ   |
| cln03 | この写真の夕焼け、すごーい | このしゃしんのゆうやけ、すごーい | このしゃしんのゆうやけ、すごーい |  | none | 長音   |
| cln04 | やっぱりそうだよね、あの店閉まるの早い | やっぱりそうだよね、あのみせしまるのはやい | やっぱりそうだよね、あのみせしまるのはやい |  | none |    |
| cln05 | 洗濯機の中で服がぐるぐる回る | せんたくきのなかでふくがぐるぐるまわる | せんたくきのなかでふくがぐるぐるまわる |  | none | 正しい繰り返し   |
| cln06 | いよいよ明日 | いよいよあした | いよいよあした |  | none | 正しい繰り返し   |
| cln07 | のどがからから | のどがからから | のどがからから |  | none | 正しい繰り返し（からから は二重打ちに見える）   |
| cln08 | ここに置いといて | ここにおいといて | ここにおいといて |  | none | 正しい繰り返し   |
| cln09 | なんでやねん、それ昨日も言うたやん | なんでやねん、それきのうもいうたやん | なんでやねん、それきのうもいうたやん |  | none | 方言   |
| cln10 | そうやけんね | そうやけんね | そうやけんね |  | none | 方言   |
| cln11 | その帽子、めっちゃええやん | そのぼうし、めっちゃええやん | そのぼうし、めっちゃええやん |  | none | 方言・母音の連続   |
| cln12 | 今日の宿題の量、まじでやばい | きょうのしゅくだいのりょう、まじでやばい | きょうのしゅくだいのりょう、まじでやばい |  | none | くだけた言い方   |
| cln13 | ぴえんすぎる | ぴえんすぎる | ぴえんすぎる |  | none | 新語   |
| cln14 | ファイルを開く | ふぁいるをひらく | ふぁいるをひらく |  | none | 小さい仮名の外来語   |
| cln15 | ウェブサイトの問い合わせ窓口 | うぇぶさいとのといあわせまどぐち | うぇぶさいとのといあわせまどぐち |  | none | 小さい仮名の外来語   |
| cln16 | ディスプレイが割れた | でぃすぷれいがわれた | でぃすぷれいがわれた |  | none | 小さい仮名の外来語   |
| cln17 | キャンセル待ちの番号が届いた | きゃんせるまちのばんごうがとどいた | きゃんせるまちのばんごうがとどいた |  | none | 外来語   |
| cln18 | ぽかぽか陽気 | ぽかぽかようき | ぽかぽかようき |  | none | 擬態語   |
| cln19 | もふもふの猫 | もふもふのねこ | もふもふのねこ |  | none | 擬態語   |
| cln20 | えっ、ほんとに | えっ、ほんとに | えっ、ほんとに |  | none | 促音で終わる感動詞   |
| cln21 | んー、どうしよう | んー、どうしよう | んー、どうしよう |  | none | ん で始まる   |
| cln22 | 明日の件はおっけー | あしたのけんはおっけー | あしたのけんはおっけー |  | none | 促音と長音   |
| cln23 | はいはい、わかった、皿洗いは僕がやる | はいはい、わかった、さらあらいはぼくがやる | はいはい、わかった、さらあらいはぼくがやる |  | none | 正しい繰り返し   |
| cln24 | 駅まで送ってくれてありがとうね | えきまでおくってくれてありがとうね | えきまでおくってくれてありがとうね |  | none |    |
| cln25 | お疲れさまでした、また来週お願いします | おつかれさまでした、またらいしゅうおねがいします | おつかれさまでした、またらいしゅうおねがいします |  | none |    |
| cln26 | 佐渡に行きたい | さどにいきたい | さどにいきたい |  | none | さど は さいど の typo に見える（CLAUDE.md の例）   |
| cln27 | 再度確認します | さいどかくにんします | さいどかくにんします |  | none |    |
| cln28 | 来週の面接もきっと大丈夫 | らいしゅうのめんせつもきっとだいじょうぶ | らいしゅうのめんせつもきっとだいじょうぶ |  | none |    |
| cln29 | ちゃっかりしてる | ちゃっかりしてる | ちゃっかりしてる |  | none | 拗音と促音   |
| cln30 | 妹は昔からしっかり者 | いもうとはむかしからしっかりもの | いもうとはむかしからしっかりもの |  | none |    |
| cln31 | 八王子駅の北口で集合 | はちおうじえきのきたぐちでしゅうごう | はちおうじえきのきたぐちでしゅうごう |  | none | 地名   |
| cln32 | さいたま市の図書館に寄る | さいたましのとしょかんによる | さいたましのとしょかんによる |  | none | 地名   |
| cln33 | 宇都宮の餃子 | うつのみやのぎょうざ | うつのみやのぎょうざ |  | none | 地名   |
| cln34 | 三時半ごろ | さんじはんごろ | さんじはんごろ |  | none | 数の読み   |
| cln35 | 百二十円 | ひゃくにじゅうえん | ひゃくにじゅうえん |  | none | 数の読み   |
| cln36 | 午前中に倉庫へ納品予定です | ごぜんちゅうにそうこへのうひんよていです | ごぜんちゅうにそうこへのうひんよていです |  | none |    |
| cln37 | 本日はお日柄もよく | ほんじつはおひがらもよく | ほんじつはおひがらもよく |  | none | 改まった言い方   |
| cln38 | ご査収ください | ごさしゅうください | ごさしゅうください |  | none | まれなビジネス語   |
| cln39 | 取り急ぎご連絡まで | とりいそぎごれんらくまで | とりいそぎごれんらくまで |  | none |    |
| cln40 | ご無沙汰しております、営業二課の田中です | ごぶさたしております、えいぎょうにかのたなかです | ごぶさたしております、えいぎょうにかのたなかです |  | none |    |
| cln41 | 検討させていただきます、来期の予算案として | けんとうさせていただきます、らいきのよさんあんとして | けんとうさせていただきます、らいきのよさんあんとして |  | none |    |
| cln42 | おかげさまで、新しい店舗も順調です | おかげさまで、あたらしいてんぽもじゅんちょうです | おかげさまで、あたらしいてんぽもじゅんちょうです |  | none |    |
| cln43 | 先行研究を概観する | せんこうけんきゅうをがいかんする | せんこうけんきゅうをがいかんする |  | none |    |
| cln44 | 二つの群の間に有意差は認められなかった | ふたつのぐんのあいだにゆういさはみとめられなかった | ふたつのぐんのあいだにゆういさはみとめられなかった |  | none |    |
| cln45 | 今回の結果は仮説を支持する | こんかいのけっかはかせつをしじする | こんかいのけっかはかせつをしじする |  | none |    |
| cln46 | 雨のせいで試合が急きょ中止になった | あめのせいでしあいがきゅうきょちゅうしになった | あめのせいでしあいがきゅうきょちゅうしになった |  | none | きゅうきょ は きょうきょ 等の typo に見える   |
| cln47 | 電車の遅延はしょっちゅうある | でんしゃのちえんはしょっちゅうある | でんしゃのちえんはしょっちゅうある |  | none | 拗音と促音   |
| cln48 | ぎりぎり間に合った | ぎりぎりまにあった | ぎりぎりまにあった |  | none | 正しい繰り返し   |
| cln49 | ささっと済ませる | ささっとすませる | ささっとすませる |  | none | 正しい繰り返し   |
| cln50 | 九日の予定 | ここのかのよてい | ここのかのよてい |  | none | ここ の繰り返し   |
| cln51 | ええと、どこまで話したっけ | ええと、どこまではなしたっけ | ええと、どこまではなしたっけ |  | none | 母音の連続   |
| cln52 | あのー、すみません | あのー、すみません | あのー、すみません |  | none |    |
| cln53 | いいえ、違います、その傘は私のです | いいえ、ちがいます、そのかさはわたしのです | いいえ、ちがいます、そのかさはわたしのです |  | none | 母音の連続   |
| cln54 | 大きい犬 | おおきいいぬ | おおきいいぬ |  | none | い が 3 つ続く   |
| cln55 | 言いたかったのはこういうこと | いいたかったのはこういうこと | いいたかったのはこういうこと |  | none |    |
| cln56 | 新入社員の挨拶が初々しい | しんにゅうしゃいんのあいさつがういういしい | しんにゅうしゃいんのあいさつがういういしい |  | none | 母音の連続   |
| cln57 | 締め切り前でいっぱいいっぱい | しめきりまえでいっぱいいっぱい | しめきりまえでいっぱいいっぱい |  | none | 正しい繰り返し   |
| cln58 | そうそう、それそれ、その青いマグカップ | そうそう、それそれ、そのあおいまぐかっぷ | そうそう、それそれ、そのあおいまぐかっぷ |  | none | 正しい繰り返し   |
| cln59 | 先日は大変お世話になって | せんじつはたいへんおせわになって | せんじつはたいへんおせわになって |  | none | 打ちかけ（この後に おります が続く）   |
| cln60 | 日程を確認させて | にっていをかくにんさせて | にっていをかくにんさせて |  | none | 打ちかけ（この後に いただきます が続く）   |
