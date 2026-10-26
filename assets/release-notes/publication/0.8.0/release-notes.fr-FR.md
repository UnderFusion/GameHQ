# GameHQ 0.8.0 (2026-10-26)

## Remerciements particuliers

- Un grand merci à [@galcn10](https://github.com/galcn10), créateur de [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), pour avoir testé GameHQ, partagé de précieux retours et créé une excellente intégration Playnite. Votre soutien a contribué à améliorer GameHQ 0.8.0 !

## Points forts

- Détection automatique des mises à jour améliorée. GameHQ recherche désormais les nouvelles versions peu après chaque démarrage, puis toutes les heures pendant son exécution, pour que vous découvriez les mises à jour bien plus tôt.
- Les clips peuvent enregistrer le son de votre jeu lui-même. Avec la nouvelle source Son du jeu, GameHQ enregistre le jeu sur l’appareil par lequel il est diffusé, quel qu’il soit (casque, Bluetooth, HDMI ou haut-parleurs), et laisse de côté les autres applications comme Discord ou Spotify.
- Les problèmes d’enregistrement audio sont désormais détectés et signalés. Si la source audio sélectionnée ne peut pas être enregistrée, le tampon de replay s’arrête et vous indique pourquoi et quelle source choisir à la place, au lieu d’enregistrer discrètement des clips sans son. GameHQ ne change jamais la source audio de lui-même.
- Le son et l’image restent synchronisés. Les clips enregistrés après un écran de chargement, une pause ou un long blocage conservent un minutage correct, et le tampon de replay redémarre de lui-même lorsqu’un jeu change de résolution.
- Les captures d’écran HDR ne restent plus bloquées. Une capture d’écran d’un jeu en pause ou d’un menu statique en HDR est enregistrée immédiatement. Si aucune image n’arrive, la tentative échoue proprement au bout de quelques secondes et l’appui suivant fonctionne.
- Suppression plus sûre à la manette. Dans une confirmation de suppression, Croix appuie désormais sur le bouton en surbrillance et Cercle annule. Auparavant, Croix pouvait supprimer le fichier même lorsque Annuler était en surbrillance.
- La suppression protège vos originaux. Une capture créée par GameHQ est désormais déplacée dans la Corbeille lorsque vous la supprimez : vous pouvez donc la restaurer. Les fichiers des dossiers surveillés ne sont jamais supprimés.

## Nouveautés

- Paramètres › Replay › Source audio propose Son du jeu, Son du bureau et Désactivé. Son du jeu est l’option par défaut sous Windows 11. Son du bureau enregistre tout ce qui passe par la sortie Windows par défaut, y compris les autres applications, et c’est l’option par défaut sur les versions plus anciennes de Windows.
- Son du jeu utilise la capture audio par application intégrée à Windows et ne touche jamais au processus du jeu. Si vous aviez volontairement désactivé le son du système dans une version précédente, il reste désactivé.
- Mises à jour bêta facultatives : un nouvel interrupteur Inclure les versions bêta dans Paramètres › Général et dans À propos permet au programme de mise à jour de proposer des versions bêta. Il est désactivé par défaut, de sorte que les installations stables ne reçoivent que des mises à jour stables.
- Lorsque GameHQ plante, il tente d’enregistrer un petit fichier de vidage sur incident dans son dossier des journaux pour aider à diagnostiquer le problème. Rien n’est envoyé. Certains plantages, comme ceux auxquels Windows met fin immédiatement, peuvent encore fermer GameHQ sans fichier de vidage.
- Le journal consigne davantage de détails sur les captures d’écran enregistrées et les échecs de démarrage, si bien qu’un seul fichier journal suffit plus souvent pour comprendre un signalement de problème.

## Corrections et améliorations

- Les clips ne sont plus muets à cause d’un ancien paramètre interne qui laissait le son du bureau désactivé par défaut.
- L’audio dans n’importe quel format de lecture Windows est correctement converti au lieu que la piste audio soit perdue.
- Les moments calmes sont enregistrés comme du silence, de sorte que le son qui suit un passage calme reste aligné sur l’image.
- L’enregistrement du son du jeu redémarre de façon plus fiable lorsqu’un jeu redémarre ou que vous changez de jeu.
- Les parties du tampon de replay qui n’ont pas pu être enregistrées ou ne peuvent pas être relues sont écartées, afin qu’elles ne puissent pas endommager un clip enregistré.
- Les notifications propres à GameHQ, comme l’avis indiquant qu’une capture d’écran a été enregistrée, n’apparaissent plus dans vos captures d’écran ni dans les enregistrements et les diffusions de l’écran entier.
- Une capture d’écran remplacée par une autre image portant le même nom de fichier reçoit une nouvelle vignette lors de la prochaine analyse de la bibliothèque.
- Les dossiers de capture sont analysés en arrière-plan, si bien que GameHQ reste réactif avec de grandes collections.
- Les navigateurs, les éditeurs et les autres fenêtres qui ne sont pas des jeux n’apparaissent plus comme des jeux dans votre bibliothèque.
- Lors d’une suppression groupée, la sélection reste figée tant que la confirmation est ouverte, de sorte que des actions de la manette en arrière-plan ne peuvent pas changer les fichiers supprimés.
- La boîte de dialogue de suppression de la surcouche reçoit les entrées de la manette tant qu’elle est ouverte et se ferme lorsque la surcouche est masquée.
- La surcouche ignore les fenêtres qui ne répondent plus au lieu de les attendre.
- La page Paramètres ne remonte plus tout en haut lorsque vous faites défiler avec la souris après être revenu sur GameHQ.
- Choisir Valeurs par défaut intégrées ou supprimer un préréglage dans l’éditeur d’assignation de la manette ne fige plus la sélection du préréglage.

## Manettes et Playnite

- Les raccourcis de la manette continuent de fonctionner dans les jeux lancés en dehors de Steam, comme un jeu local démarré depuis Playnite. Lorsque Windows cesse de transmettre à GameHQ une DualSense connectée pendant que le jeu a le focus, GameHQ lit la manette directement, en lecture seule, et revient à l’entrée normale dès que Windows la transmet de nouveau.
- Les raccourcis et les répétitions de bouton maintenu se terminent toujours au relâchement de la même pression, y compris lorsque le relâchement est signalé via GameInput ou que la manette est débranchée pendant qu’un bouton est maintenu.
- Les manettes déconnectées définitivement sont nettoyées, la vérification HidHide ne ralentit plus l’interface, et les périphériques GameInput ne sont revérifiés que lorsque quelque chose change.
- Les jeux Playnite restent reconnus même lorsque Playnite ne parvient pas à identifier le processus du jeu.
- Expérience plein écran Xbox de Windows : lorsque l’interface Xbox est au premier plan, GameHQ utilise le jeu lancé depuis Playnite pour les captures d’écran et les clips. GameHQ Playnite Integration 0.4.13, disponible séparément en préversion, démarre également GameHQ lorsque le mode Xbox retarde son lancement à l’ouverture de session.

## Stabilité et fiabilité

- Le nettoyage des enregistrements inachevés ne supprime que les fichiers temporaires créés par GameHQ lui-même.
- Les dossiers de capture des jeux restent toujours à l’intérieur de votre dossier de capture.
- Le démarrage n’échoue plus lorsque la bibliothèque contient des chemins de dossier en double ; les entrées sont fusionnées à la place.
- Si GameHQ ne peut pas démarrer, il en explique la raison dans votre langue et la consigne dans le journal.
- L’importation d’un profil portable dans un GameHQ installé fonctionne désormais lorsque la copie installée a déjà été lancée.
- Sécurité renforcée : chargement des DLL plus sûr, contrôles plus stricts des connexions d’intégration locales et gestion plus sûre des fichiers de mise à jour.
- Le programme de mise à jour gère mieux un premier démarrage lent et conserve ses données intactes s’il est interrompu.
- Le partage vers Telegram s’arrête de manière plus sûre.

## Installation et mise à jour

- Mise à jour depuis la 0.7.9 : exécutez le programme d’installation de la 0.8.0 par-dessus votre installation actuelle.
- Programme d’installation : installe pour l’utilisateur actuel et remplace une version 0.7.9 installée ou une bêta antérieure.
- Version portable : extrayez le ZIP dans un nouveau dossier et lancez GameHQ.exe.
- Si vous avez testé la 0.8.0 Beta 2, 3 ou 4, installez la 0.8.0 avec le programme d’installation ou un nouveau ZIP portable, car ces bêtas n’ont pas été proposées par le programme de mise à jour.

## Limitations connues

- Son du jeu nécessite Windows 11. Sur les versions plus anciennes de Windows, GameHQ utilise Son du bureau, qui enregistre aussi les autres applications diffusées sur la sortie par défaut.
- Un jeu qui prend le contrôle exclusif d’une manette peut encore empêcher les raccourcis en arrière-plan.
- La prise en charge de l’expérience plein écran Xbox de Windows n’a pas encore été confirmée par des testeurs sur un PC qui l’utilise.
- Les binaires Windows ne sont pas signés avec Authenticode ; Windows SmartScreen peut afficher un avertissement Éditeur inconnu.
