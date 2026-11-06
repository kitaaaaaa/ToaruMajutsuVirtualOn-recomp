"""Writes the distributed "English": a deliberately terrible translation, in the spirit of Zero Wing's opening,
bootleg comics and dubbed kung fu films. It's a joke, not a translation. Hand-written literal menus stay as they
are (HAND); everything else in the game's text comes from here, and can't be followed.

Usage: wacky.py <dump_dir> <translation_dir>

<dump_dir> holds `mtp.py dump` output, from your own copy of the game. Only each line's shape is used: its tags,
its punctuation, its length, and its katakana words, romanized as they stand. The result is English only and
safe to publish. The same input always gives the same output.
"""
import glob
import os
import re
import sys
import zlib

HAND = {
    "chara_name_01", "chara_name_backlog_01", "common_msg_01", "dialog_msg_01", "menu_msg_01", "mission_category_name_01",
    "mission_clear_rank_msg_01", "mission_msg_01", "mission_rule_name_01", "option_msg_01", "option_value_01",
    "overview_title_name_01", "replay_sort_name_01", "result_msg_01", "ririna_msg_01", "stage_name_01", "story_msg_01",
    "story_title_01", "terminal_msg_01", "warning_name_01",
}

STATEMENTS = [
    "All your {n} is belong to the honorable me.",
    "Somebody set up us the {n}.",
    "You have no chance to survive make your {n}.",
    "For great justice, I have ate the {n}.",
    "This {n} is very danger of the delicious.",
    "My father's {n} did not teach me this feeling.",
    "It is the wind of {n} that blow the pants of destiny.",
    "Good morning! Today {n} is also exploded.",
    "I shall punch the {n} with my whole heart and also the spleen.",
    "The {n} goes to the sea and never pay back the money.",
    "Please be careful, the {n} is standing behind of your yesterday.",
    "Without {n}, a man is only two sandwiches.",
    "I have trained in the mountain for 800 {n}.",
    "That is not a {n}, that is my brother.",
    "Everyone is laugh, but the {n} is crying inside the soup.",
    "Listen well! A {n} who is run away can not eat of rice.",
    "The sky is angry because of your {n}.",
    "Long ago, the {n} was only small boy with a dream of noodle.",
    "I will now do the secret technique of {n}, please hold my bag.",
    "According to the rule of {n}, you are already a Tuesday.",
    "It is too late, the {n} has learned to walk.",
    "My {n} is overflow and I can't stopping it.",
    "We fight because of the {n}, but the {n} does not love us.",
    "Do not worry. The {n} is delicious, and it is also your uncle.",
    "This is the {n} of the legend. It cost 300 yen.",
    "Excuse me, your {n} is on fire with the sincerity.",
    "Yesterday the {n}, today the {n}, tomorrow maybe a nice fish.",
    "Hmm. I understand everything except for the {n}, and also the rest.",
    "He who controls the {n} will be having a very busy Sunday.",
    "The robot is sad, it have forgotten the {n} in the bus.",
]
QUESTIONS = [
    "What you say about {n}?",
    "How are you {n} gentlemen?",
    "Is this the {n}, or is this the Thursday?",
    "Why does your {n} look at me with such a face?",
    "Who put the {n} into my important shoes?",
    "Do you really think that {n} will forgive the rice?",
    "Can a man eat {n} and still be called a man of the bus?",
    "Where has gone the {n} of our promise?",
    "Is it too much {n}? Or too few of the feelings?",
    "How much is this {n} if I don't want it?",
    "Have you been told that your {n} is a duck?",
    "Could it be... the {n} has been a stairs all along?",
]
SHOUTS = [
    "Take off every {n}!!",
    "Move {n} for great justice!!",
    "It's you! The {n} of the crooked pants!",
    "Ora ora ora, {n} ora!",
    "Your {n} is weak, but your grandmother is strong!",
    "I am become the {n}! Please applaud!",
    "So fast! Too much {n}! My eye!",
    "Impossible! He is having the {n} of two men!",
    "Noooo! Not the {n}! I just washed it!",
    "Now is the time of {n}! Everybody go home!",
    "You fight like {n}! Very nice, thank you!",
    "Haaa! Ultra {n} kick from the side of a building!",
    "Wow! Such a {n}! I am surprise to the bone!",
    "Stop it! The {n} is already dead and it is angry!",
]
TITLES = [
    "Great {n}", "{n} of Justice", "The Delicious {n}", "Angry {n}", "{n} Is Belong", "Secret {n} Technique", "Honorable {n}",
    "Sad {n} Legend", "{n} for Great Justice", "Ultra {n} DX", "Bus of {n}", "Yesterday's {n}", "Crooked {n}", "{n} (Danger)",
    "Very {n}", "{n} and Rice", "Super Sorry {n}", "Fist of {n}", "{n} Mountain", "Not a {n}",
]
TUTORIAL = [
    "When the {n} is come, press {k} for great justice.",
    "Use {k} and the {n} will respect your family.",
    "If your {n} is crying, it is time of {k}.",
    "Press {k} now. Do not ask the {n} why.",
    "The master says: {k} is the way of {n}.",
    "Beginner always forget {k}, then the {n} eat them.",
]
NOUNS = [
    "base", "zig", "bomb", "rice", "noodle", "kettle", "pants", "fist", "justice", "soup", "bicycle", "destiny", "tofu", "uncle",
    "shrimp", "robot", "thunder", "dumpling", "receipt", "ladder", "turnip", "umbrella", "ghost", "trombone", "elbow", "pigeon",
]
SIGHS = ["...", "......", "...hm.", "...!?", "..."]

KANA = {
    "ア": "A", "イ": "I", "ウ": "U", "エ": "E", "オ": "O", "カ": "KA", "キ": "KI", "ク": "KU", "ケ": "KE", "コ": "KO", "ガ": "GA", "ギ": "GI",
    "グ": "GU", "ゲ": "GE", "ゴ": "GO", "サ": "SA", "シ": "SHI", "ス": "SU", "セ": "SE", "ソ": "SO", "ザ": "ZA", "ジ": "JI", "ズ": "ZU", "ゼ": "ZE",
    "ゾ": "ZO", "タ": "TA", "チ": "CHI", "ツ": "TSU", "テ": "TE", "ト": "TO", "ダ": "DA", "ヂ": "JI", "ヅ": "ZU", "デ": "DE", "ド": "DO", "ナ": "NA",
    "ニ": "NI", "ヌ": "NU", "ネ": "NE", "ノ": "NO", "ハ": "HA", "ヒ": "HI", "フ": "FU", "ヘ": "HE", "ホ": "HO", "バ": "BA", "ビ": "BI", "ブ": "BU",
    "ベ": "BE", "ボ": "BO", "パ": "PA", "ピ": "PI", "プ": "PU", "ペ": "PE", "ポ": "PO", "マ": "MA", "ミ": "MI", "ム": "MU", "メ": "ME", "モ": "MO",
    "ヤ": "YA", "ユ": "YU", "ヨ": "YO", "ラ": "RA", "リ": "RI", "ル": "RU", "レ": "RE", "ロ": "RO", "ワ": "WA", "ヲ": "O", "ン": "N", "ヴ": "VU",
}
SMALL = {"ァ": "A", "ィ": "I", "ゥ": "U", "ェ": "E", "ォ": "O", "ャ": "YA", "ュ": "YU", "ョ": "YO"}


def romanize(word):
    out = ""
    double = False
    for c in word:
        if c == "ッ":
            double = True
        elif c == "ー":
            out += out[-1] if out and out[-1] in "AIUEO" else ""
        elif c in SMALL:
            small = SMALL[c]
            if out and out[-1] in "AIUEO" and len(out) > 1 and small[0] != "Y":
                out = out[:-1] + small
            elif out and out[-1] in "IAUEO" and small[0] == "Y":
                head = out[:-1]
                out = (head[:-1] + "SH" + small[1:]) if head.endswith("SH") else head + small
            else:
                out += small
        elif c in KANA:
            syllable = KANA[c]
            out += (syllable[0] if double and syllable[0] not in "AIUEON" else "") + syllable
            double = False
    return out


def nouns_of(text):
    plain = re.sub(r"<[^>]*>", "", text)
    words = [romanize(w) for w in re.findall(r"[ァ-ヺー]{3,}", plain)]
    return [w for w in words if len(w) >= 3]


# The story's sport. Nobody at the translation company ever agreed how to spell it.
SPORT = "バーチャロン"
SPORT_SPELLINGS = [
    "virtual on", "virtualon", "Virtual on", "virtual On", "VIRTUAL ON", "virtul on", "virtuel on", "vertual on", "Virtualon",
    "virtula on", "virchual on", "Vertual on", "virtual onn", "virtua lon", "VirtualOn", "virtuall on", "virtal on", "Virtuel On",
    "birtual on", "virtual-on", "virtual oh", "virtual un",
]


def is_japanese(text):
    return any(ord(c) > 0x2FFF for c in text)


def generate(kind, text):
    seed = zlib.crc32((kind + text).encode("utf-8"))
    # Whole-line effects, and the codes the game fills in or draws, have to survive.
    plain = re.sub(r"<ruby [^>]*>|</ruby>", "", text)
    color = re.search(r"<color [^>]*>", plain)
    effect = next((e for e in ("double", "shake") if "<%s>" % e in plain), None)
    tokens = re.findall(r"%[-0-9.]*l*[a-zA-Z]|<ARG>|<input [^>]*>|<icon [^>]*>", plain)
    bare = re.sub(r"<[^>]*>|\\n|%[-0-9.]*l*[a-zA-Z]", "", plain).strip()

    words = nouns_of(text)
    if (SPORT in text or any("BAACHYARON" in w for w in words)) and kind.startswith(("story", "DLC", "TRIAL")):
        # Mention the sport, and spell it however this line feels like. Pool of one: always the sport.
        words = [SPORT_SPELLINGS[(seed >> 7) % len(SPORT_SPELLINGS)]]

    def noun(step):
        pool = words if words else NOUNS
        return pool[(seed >> step) % len(pool)] if words else NOUNS[(seed >> step) % len(NOUNS)]

    inputs = [t for t in tokens if t.startswith("<input") or t.startswith("<icon")]
    if bare and not re.search(r"[^…。、・．.！？!? 　]", bare) and not tokens:
        result = SIGHS[seed % len(SIGHS)]
    elif inputs and kind.startswith(("tutorial", "TRIAL")):
        result = TUTORIAL[seed % len(TUTORIAL)].format(n=noun(3), k=inputs[0])
        tokens.remove(inputs[0])
    else:
        length = len(bare)
        if "？" in bare or "?" in bare:
            bank = QUESTIONS
        elif "！" in bare or "!" in bare:
            bank = SHOUTS
        elif length <= 12 and "。" not in bare:
            bank = TITLES
        else:
            bank = STATEMENTS
        result = bank[seed % len(bank)].replace("{n}", noun(5))
    for token in tokens:
        result += " " + token
    # Dialogue boxes wrap by hand: break long lines where the original would.
    if len(result) > 44 and " " in result:
        middle = len(result) // 2
        left = result.rfind(" ", 0, middle + 1)
        right = result.find(" ", middle)
        cut = left if right < 0 or (left >= 0 and middle - left <= right - middle) else right
        if not any(result.rfind("<", 0, cut) > result.rfind(">", 0, cut) for _ in [0]):
            result = result[:cut] + "\\n" + result[cut + 1 :]
    if effect:
        result = "<%s>%s</%s>" % (effect, result, effect)
    if color:
        result = "%s%s</color>" % (color.group(0), result)
    return result


def main():
    dump, out = sys.argv[1:3]
    files = lines = 0
    for path in sorted(glob.glob(os.path.join(dump, "*.txt"))):
        name = os.path.splitext(os.path.basename(path))[0]
        kind = name.replace("MTPA_", "", 1)
        if kind in HAND or name.startswith("_"):
            continue
        # The voice gallery's text lists are left alone on purpose.
        if kind.startswith("voice_"):
            target = os.path.join(out, name + ".en")
            if os.path.exists(target):
                os.remove(target)
            continue
        result = []
        with open(path, encoding="utf-8") as f:
            for line in f:
                ident, tab, text = line.rstrip("\n").partition("\t")
                if tab and ident.isdigit() and is_japanese(text):
                    result.append("%s\t%s\n" % (ident, generate(kind, text)))
        target = os.path.join(out, name + ".en")
        if result:
            with open(target, "w", encoding="ascii", newline="\n") as f:
                f.writelines(result)
            files += 1
            lines += len(result)
        elif os.path.exists(target):
            os.remove(target)
    print("%d files, %d lines" % (files, lines))


if __name__ == "__main__":
    main()
