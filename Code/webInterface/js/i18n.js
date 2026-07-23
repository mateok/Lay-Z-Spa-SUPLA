// i18n.js

const LANG_STORAGE_KEY = 'langueSPA'
const DEFAULT_LANG = 'pl-PL'

function getTranslation(key, fallback = key) {
    if (window.translations && window.translations[key]) {
        return window.translations[key]
    }
    return fallback
}

function getLangueChoisie() {
    const stored = localStorage.getItem(LANG_STORAGE_KEY)
    return stored ? stored : DEFAULT_LANG
}

function setLangueChoisie(lang) {
    localStorage.setItem(LANG_STORAGE_KEY, lang)
}

function chargerFichierTraduction(lang, callback) {
    const url = 'Langue_' + lang + '.txt'
    fetch(url)
        .then(response => {
            if (!response.ok) {
                callback(null)
            } else {
                return response.text()
            }
        })
        .then(text => {
            if (text !== undefined) callback(text)
        })
        .catch(() => callback(null))
}

function parserTraduction(contenu) {
    const lignes = contenu.split(/\r?\n/)
    const dic = {}
    lignes.forEach(ligne => {
        if (!ligne || ligne.startsWith('#')) return
        const idx = ligne.indexOf('=')
        if (idx > 0) {
            const key = ligne.slice(0, idx).trim()
            const val = ligne.slice(idx + 1).trim()
            dic[key] = val
        }
    })
    return dic
}


function appliquerTraduction(dic) {
    window.translations = dic
    document.querySelectorAll('[data-i18n]').forEach(elem => {
        const cle = elem.getAttribute('data-i18n')

        // Si la clé commence par [placeholder], on traduit le placeholder
        if (cle.startsWith('[placeholder]')) {
            const vraieCle = cle.replace(/^\[placeholder\]/, '')
            if (vraieCle in dic) {
                elem.setAttribute('placeholder', dic[vraieCle])
            }
            return
        }

        // Si la clé commence par [data-text], on traduit l’attribut data-text
        if (cle.startsWith('[data-text]')) {
            const vraieCle = cle.replace(/^\[data-text\]/, '')
            if (vraieCle in dic) {
                elem.setAttribute('data-text', dic[vraieCle])
            }
            return
        }

        // Sinon, on traduit l’innerHTML (texte visible)
        if (cle in dic) {
            elem.innerHTML = dic[cle]
        }
    })
}



function initI18n() {
    const selectLang = document.getElementById('lang-select')
    const langueActuelle = getLangueChoisie()

    // Si on est sur une page qui a le <select> (webconfig.html), on positionne et on écoute le changement
    if (selectLang) {
        selectLang.value = langueActuelle
        selectLang.addEventListener('change', () => {
            const nouvelleLang = selectLang.value
            setLangueChoisie(nouvelleLang)
            chargerFichierTraduction(nouvelleLang, contenu => {
                if (contenu) appliquerTraduction(parserTraduction(contenu))
            })
        })
    }

    // Every language, including the default one, is a real translation file.
    // This keeps Polish as the deterministic factory language while preserving
    // the existing language selector for users who prefer another language.
    chargerFichierTraduction(langueActuelle, contenu => {
        if (contenu) appliquerTraduction(parserTraduction(contenu))
    })
}

document.addEventListener('DOMContentLoaded', initI18n)
