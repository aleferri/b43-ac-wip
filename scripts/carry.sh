# shellcheck shell=sh
# Rigenerazione delle patch per merge a tre vie, incluso da regen-patches.sh e
# phase1-to-openwrt.sh.
#
# Le patch di una serie sono, sulla loro base (un tag del kernel, backports
# con le patch di OpenWrt), l'albero del port piu' gli adattamenti a quella
# base che l'albero non ha. Rigenerarle non e' quindi una diff fra la base e
# l'albero: si applicano le patch correnti alla base, e sopra si porta solo
# quello che e' cambiato nell'albero da quando erano state generate. Il
# merge a tre vie ha per base comune l'albero registrato in $CARRY_BASEFILE,
# per un lato la base con le patch correnti, per l'altro l'albero di HEAD.
# Dove il port cambia accanto a un adattamento il merge si ferma, e si
# risolve a mano nella directory di lavoro, che resta (vedi carry_run).
#
# Il chiamante definisce:
#   carry_map TREE   per ogni file dell'albero TREE (righe "directory id",
#                    vedi carry_tree), una riga
#                    "percorso-nel-repo percorso-nella-base n", con n il
#                    numero della patch (1..) che lo porta
#   carry_filter     filtro stdin -> stdout applicato a ogni file dell'albero
#   carry_base_files riempie $WORK/tree con i file della base; riceve su
#                    stdin i percorsi nella base che servono
# e imposta REPO, CARRY_BASEFILE, CARRY_TOPS (le directory dell'albero
# registrate) e CARRY_PATCHES (le patch correnti, in ordine) prima di
# chiamare carry_run o carry_resume.

carry_die() {
	echo "$*" >&2
	exit 1
}

carry_git() {
	git -C "$WORK/tree" -c core.autocrlf=false -c diff.noprefix=false \
		-c diff.mnemonicPrefix=false -c diff.renames=false \
		-c merge.renames=false "$@"
}

# Valore di una chiave del file dei riferimenti.
carry_base_get() {
	[ -f "$CARRY_BASEFILE" ] || carry_die "$CARRY_BASEFILE assente"
	sed -n "s|^$1[[:space:]]\{1,\}||p" "$CARRY_BASEFILE"
}

# L'albero per carry_map: "<top> <id>" per ogni directory registrata, o di
# HEAD con $1 = HEAD.
carry_tree() {
	for top in $CARRY_TOPS; do
		if [ "$1" = HEAD ]; then
			id=$(git -C "$REPO" rev-parse "HEAD:$top")
		else
			id=$(carry_base_get "$top")
			[ -n "$id" ] || carry_die "$CARRY_BASEFILE: manca $top"
			git -C "$REPO" cat-file -e "$id" 2>/dev/null ||
				carry_die "$CARRY_BASEFILE: l'albero $top $id non e' nel repo"
		fi
		echo "$top $id"
	done
}

# Contenuto di un percorso del repo nell'albero dato da carry_tree.
carry_show() {
	echo "$1" | while read -r top id; do
		case $2 in
		"$top"/*) git -C "$REPO" show "$id:${2#"$top"/}"; return ;;
		esac
	done
}

carry_check_clean() {
	for top in $CARRY_TOPS; do
		git -C "$REPO" diff --quiet HEAD -- "$top" ||
			carry_die "$top/ ha modifiche non committate"
		[ -z "$(git -C "$REPO" ls-files --others --exclude-standard -- "$top")" ] ||
			carry_die "$top/ ha file non tracciati"
	done
}

# Scrive nell'albero di lavoro i file di una mappa, letti dall'albero $1.
carry_write() {
	while read -r src dst n; do
		mkdir -p "$WORK/tree/$(dirname "$dst")"
		carry_show "$1" "$src" | carry_filter > "$WORK/tree/$dst"
	done < "$2"
}

carry_run() {
	carry_check_clean
	old=$(carry_tree base)
	new=$(carry_tree HEAD)
	WORK=$(mktemp -d)
	mkdir "$WORK/tree"
	carry_keep=
	trap '[ -n "$carry_keep" ] || rm -rf "$WORK"' EXIT

	carry_map "$old" > "$WORK/map.old"
	carry_map "$new" > "$WORK/map.new"
	# Un file uscito dall'albero torna alla base nella patch che lo portava.
	{
		cat "$WORK/map.new"
		awk 'NR == FNR { seen[$2] = 1; next } !($2 in seen)' \
			"$WORK/map.new" "$WORK/map.old"
	} > "$WORK/map.all"

	carry_git init -q
	carry_git config user.name "b43-ac regen"
	carry_git config user.email "regen@localhost"
	awk '{ print $2 }' "$WORK/map.all" | carry_base_files
	carry_git add -A
	carry_git commit -qm base
	carry_git branch base

	for p in $CARRY_PATCHES; do
		carry_git am -q "$p" ||
			carry_die "$(basename "$p") non si applica alla base"
	done
	carry_git branch current

	carry_git checkout -q --orphan side
	carry_git rm -rq --cached . >/dev/null
	find "$WORK/tree" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
	carry_write "$old" "$WORK/map.old"
	carry_git add -A
	carry_git commit -qm "albero registrato"
	find "$WORK/tree" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
	carry_write "$new" "$WORK/map.new"
	carry_git add -A
	carry_git commit -q --allow-empty -m "albero di HEAD"

	carry_git checkout -q -f current
	if ! carry_git diff --quiet side~1 side; then
		if ! carry_git cherry-pick side >/dev/null 2>&1; then
			carry_keep=1
			echo "conflitti nel portare l'albero sulle patch correnti:" >&2
			carry_git diff --name-only --diff-filter=U >&2
			cat >&2 <<-EOF
			Risolvi in $WORK/tree, poi
			  git -C $WORK/tree add -A
			  git -C $WORK/tree -c core.editor=true cherry-pick --continue
			e rilancia con CARRY_RESUME=$WORK.
			EOF
			exit 1
		fi
	fi
	carry_finish
}

# Riprende dopo la risoluzione dei conflitti di carry_run. La directory
# resta finche' il chiamante non chiama carry_done.
carry_resume() {
	carry_check_clean
	WORK=$CARRY_RESUME
	carry_keep=1
	trap '[ -n "$carry_keep" ] || rm -rf "$WORK"' EXIT
	[ -d "$WORK/tree/.git" ] || carry_die "$WORK non e' una directory di carry_run"
	[ -z "$(carry_git diff --name-only --diff-filter=U)" ] &&
		! carry_git rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null ||
		carry_die "cherry-pick non concluso in $WORK/tree"
	new=$(carry_tree HEAD)
	carry_map "$new" | cmp -s - "$WORK/map.new" ||
		carry_die "l'albero di HEAD e' cambiato dal conflitto: rilancia da capo"
	carry_finish
}

carry_done() {
	carry_keep=
}

# Divide il risultato del merge nelle patch, con messaggio, autore e data di
# quelle correnti, in $WORK/out.
carry_finish() {
	merged=$(carry_git rev-parse HEAD)
	carry_git checkout -q -f base
	carry_git checkout -q -B out

	n=0
	for p in $CARRY_PATCHES; do
		n=$((n + 1))
		awk -v n="$n" '$3 == n { print $2 }' "$WORK/map.all" |
		while read -r dst; do
			if carry_git cat-file -e "$merged:$dst" 2>/dev/null; then
				mkdir -p "$WORK/tree/$(dirname "$dst")"
				carry_git show "$merged:$dst" > "$WORK/tree/$dst"
			else
				rm -f "$WORK/tree/$dst"
			fi
		done
		carry_git add -A
		carry_git mailinfo "$WORK/msg" /dev/null < "$p" > "$WORK/info"
		field() { sed -n "s/^$1: //p" "$WORK/info"; }
		{ field Subject; echo; cat "$WORK/msg"; } > "$WORK/commitmsg"
		GIT_AUTHOR_NAME=$(field Author) GIT_AUTHOR_EMAIL=$(field Email) \
		GIT_AUTHOR_DATE=$(field Date) GIT_COMMITTER_DATE=$(field Date) \
			carry_git commit -q --allow-empty -F "$WORK/commitmsg"
	done

	carry_git diff --quiet "$merged" HEAD ||
		carry_die "il merge cambia file che nessuna patch porta:
$(carry_git diff --name-only "$merged" HEAD)"

	rm -rf "$WORK/out"
	carry_git format-patch -q --zero-commit --no-signature --no-renames \
		-o "$WORK/out" base..HEAD
}

# Registra in $CARRY_BASEFILE gli alberi di HEAD, con le righe $@ in testa.
carry_record() {
	{
		for l in "$@"; do echo "$l"; done
		carry_tree HEAD
	} > "$CARRY_BASEFILE"
}
