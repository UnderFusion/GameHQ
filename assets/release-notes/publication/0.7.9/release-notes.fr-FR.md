# GameHQ 0.7.9 (2026-10-02)

## Points forts

- Partagez vos captures. Envoyez une capture d’écran ou un clip depuis la galerie, la visionneuse plein écran ou la surcouche vers Telegram Desktop, Discord, un canal Discord ou le presse-papiers, sans vous connecter à quoi que ce soit via GameHQ.
- Une DualSense fiable en USB et en Bluetooth. Les boutons et les sticks sont correctement lus en Bluetooth, passer du câble au Bluetooth ne nécessite plus de redémarrage, et débrancher la manette pendant une partie ne fait plus planter GameHQ. Les manettes DualSense et DualShock 4 masquées par HidHide restent utilisables dès que GameHQ est autorisé dans HidHide.
- Une surcouche à votre mesure. Un panneau Options de la superposition, appliqué en direct, règle les marges, l’espacement, la taille des miniatures, la taille de l’interface et l’aide des commandes, et la barre latérale peut être développée, réduite, réglée sur Auto ou redimensionnée.
- Affichage plein écran dans la surcouche. Ouvrez une capture d’écran en plein écran par-dessus votre jeu, parcourez les captures d’écran et les clips avec L1/R1 et ajoutez des favoris depuis la visionneuse.
- Une galerie qui suit vos fichiers. Les captures supprimées, restaurées ou copiées dans l’Explorateur de fichiers apparaissent et disparaissent immédiatement, et les jeux peuvent être épinglés en haut de la liste.
- Des fichiers de capture d’écran plus légers. Le JPEG à 90 % de qualité est désormais le format par défaut, le PNG reste disponible.
- Plus de contrôle sur les notifications. Les avis de demande de capture, de conflit Steam Input et de manette masquée peuvent être réglés individuellement, et les utilisateurs de la souris peuvent fermer les notifications plus tôt.
- Une plus grande partie de l’interface fonctionne avec une manette, notamment le sélecteur de langue, Soutenir GameHQ, Outils et chaque étape de Partager.

## Partage

- Partagez n’importe quelle capture d’écran ou n’importe quel clip enregistré : appuyez sur Carré sur une capture et choisissez Partager, cliquez sur l’icône Partager d’une vignette de capture ou utilisez le bouton Partager de la visionneuse plein écran. Cela fonctionne de la même façon dans la galerie, la visionneuse et la surcouche, et s’utilise entièrement à la manette (Croix sélectionne, Cercle revient en arrière).
- Telegram Desktop ouvre son propre sélecteur de discussion avec ce seul fichier. GameHQ ne se connecte jamais à Telegram.
- Discord copie la capture, ouvre Discord et vous laisse la coller dans la discussion de votre choix. GameHQ ne se connecte jamais à Discord.
- Canaux Discord publie une capture directement dans un canal via un lien de webhook ajouté dans Paramètres › Partage. Épinglez les canaux que vous utilisez le plus. La publication apparaît au nom du webhook, et non de votre compte Discord, et le lien est conservé dans le Gestionnaire d’identification de Windows.
- Copier dans le presse-papiers est disponible comme destination à part entière.
- Lorsque vous partagez depuis la surcouche, Telegram ou Discord passe au premier plan par-dessus votre jeu pour que vous puissiez terminer l’envoi.
- GameHQ n’indique Envoyée que lorsqu’un service confirme la remise, et vous demande confirmation avant de partager deux fois la même capture au même endroit.
- Paramètres › Partage est une nouvelle page avec un interrupteur pour chaque destination ; en désactiver une conserve sa configuration.
- D’autres programmes de votre PC peuvent ajouter leurs propres destinations de partage. Cette option est désactivée par défaut : activez Autoriser les modules complémentaires d’autres programmes dans Paramètres › Partage, puis redémarrez GameHQ.

## Surcouche

- Croix sur une capture d’écran l’ouvre en plein écran. L1/R1 ou la croix directionnelle gauche/droite permettent de parcourir les captures d’écran et les clips (les clips sont lus en plein écran), et Cercle revient au bandeau des captures, sur l’élément où vous vous êtes arrêté. Croix sur un clip le lit toujours dans l’aperçu, puis gauche/droite permettent de s’y déplacer.
- Un engrenage au-dessus du bouton de la barre latérale ouvre les Options de la superposition : aide des commandes activée ou désactivée, les quatre marges extérieures (jusqu’à 0), la taille de l’interface (désormais avec 75 % et 90 %), la taille des miniatures et l’espacement entre la barre latérale, les miniatures et l’aperçu. Les modifications s’affichent en direct et sont mémorisées ; Réinitialiser la disposition de la superposition rétablit les valeurs par défaut. Fonctionne avec la manette, la souris et le clavier.

## Galerie et interface

- Épinglez des jeux : survolez un jeu dans la barre latérale et cliquez sur l’épingle, ou placez le curseur de la manette dessus et appuyez sur Triangle. Les jeux épinglés restent en haut de la liste, dans la fenêtre principale comme dans la surcouche.
- Dans la visionneuse plein écran, Triangle (ou le bouton que vous avez affecté aux favoris) ajoute la capture d’écran ou le clip affiché aux favoris, puis l’en retire. Un cœur indique l’état et peut être cliqué.
- Barre latérale repliable dans la fenêtre principale et dans la surcouche. Un bouton alterne entre Développée (par défaut), Auto et Réduite. Auto affiche un étroit rail d’icônes pendant la navigation et ouvre la barre latérale complète lorsque vous y entrez. Chaque fenêtre mémorise son propre choix.
- Faites glisser le bord de la barre latérale pour la rendre plus étroite ou plus large ; double-cliquez sur le bord pour rétablir la largeur par défaut.
- Une petite flèche au-dessus d’Outils replie Aide, À propos, Soutenir GameHQ et le sélecteur de langue, en ne laissant que Paramètres et le bouton de mode de la barre latérale.

## Modifications et améliorations

- Les captures d’écran sont enregistrées par défaut en JPEG à 90 % de qualité au lieu du PNG, ce qui rend chaque fichier plusieurs fois plus léger. Si vous n’avez jamais modifié le format, vous passez automatiquement au JPEG ; le PNG reste disponible dans Paramètres › Capture.
- La galerie et la surcouche suivent vos dossiers de capture en direct. Une capture supprimée dans l’Explorateur de fichiers disparaît immédiatement, et lorsqu’elle est restaurée depuis la Corbeille, elle revient avec son statut de favori et son historique intacts. Les nouveaux fichiers copiés dans un dossier de capture apparaissent sans nouvelle analyse. Une visionneuse ouverte reste sur la capture qu’elle affiche, et une confirmation de suppression supprime toujours la capture qu’elle a nommée.
- Paramètres › Notifications et son propose des interrupteurs distincts pour les avis Demande de capture reçue, Conflit Steam Input et Manette masquée.
- Lorsque vous utilisez la souris, les notifications affichent un X pour les fermer plus tôt, et les survoler les maintient à l’écran. Rien ne change pour le jeu à la manette uniquement, et un jeu qui masque le curseur n’affiche jamais le X.
- Les notifications d’avertissement restent à l’écran pendant 10 secondes et portent une icône d’avertissement.
- Les nouveaux textes concernant le partage, l’interface, la manette et les notifications ont été relus dans toutes les langues d’interface prises en charge.
- La visionneuse plein écran de la fenêtre principale utilise des marges deux fois plus petites, si bien que les captures apparaissent plus grandes.
- Les indications de la visionneuse plein écran passent aux touches fléchées et à Échap dès que vous utilisez le clavier ou la souris.
- Le sélecteur de langue, Soutenir GameHQ et le groupe Outils de la barre latérale principale sont accessibles à la manette. Dans la barre latérale réduite, le sélecteur de langue reste disponible sous forme d’icône.
- Les menus de capture, Partager et les Options de la superposition sont désormais prioritaires sur la lecture vidéo dans la surcouche, de sorte que Croix sélectionne l’entrée du menu au lieu de lire le clip situé derrière.

## Corrections de bugs

- DualSense en Bluetooth : les sticks et les boutons sont correctement lus dans GameHQ. Les rapports Bluetooth étaient lus avec un décalage d’un octet, si bien que les mouvements haut/bas des sticks étaient interprétés comme gauche/droite et que les boutons étaient mal lus. L’USB n’était pas concerné.
- L3 et R3 (clics des sticks) fonctionnent désormais sur les manettes DualSense et DualShock 4 lues directement par GameHQ.
- Passer une manette de l’USB au Bluetooth, ou inversement, pendant que GameHQ est en cours d’exécution ne la laisse plus sans réponse.
- Débrancher une manette pendant une capture en jeu ne fait plus planter GameHQ.
- Les appuis brefs ne sont plus ignorés sur les manettes qui utilisent la solution de repli joystick de Windows.
- La fermeture de la surcouche ne se bloque plus lorsqu’un bouton était déjà maintenu avant son ouverture.
- Paramètres › Entrées › Préréglages d’affectation : la liste Affectations affiche et modifie désormais le préréglage choisi sous Préréglage en cours de modification. Auparavant, les préréglages utilisés par un jeu ou non affectés à cette manette ne pouvaient pas être modifiés, et les affectations personnalisées, comme un double appui, ne pouvaient être ni supprimées ni rétablies.
- Démarrer avec Windows : une autre copie de GameHQ (une version portable ou une copie avec ses propres paramètres) ne supprime plus et ne reprend plus l’entrée de démarrage de la copie installée, ce qui pouvait empêcher la version installée de GameHQ de démarrer après un redémarrage.

## Manettes et compatibilité

- Les manettes Sony sont lues par le chemin d’entrée standard de Windows, comme auparavant.
- Une DualSense ou une DualShock 4 masquée par HidHide, en USB ou en Bluetooth, est détectée et signalée. Une fois GameHQ autorisé dans HidHide (Corriger automatiquement s’en charge), GameHQ lit la manette directement, comme le fait DSX, tandis qu’elle reste masquée pour les jeux.
- Lorsqu’un pilote de masquage de manettes (HidHide, installé avec DSX, DS4Windows ou reWASD) masque votre manette à GameHQ, une barre en haut de la fenêtre principale l’explique et propose Corriger automatiquement, Paramètres et Plus tard.
- GameHQ n’installe ni ne supprime HidHide et ne modifie pas les manettes qu’il masque. Si vous choisissez Corriger automatiquement, GameHQ s’ajoute uniquement aux applications autorisées de HidHide.
- Conseils Steam Input : lorsqu’un jeu Steam est en cours d’exécution et qu’un raccourci GameHQ utilise Create / Share ou PS, GameHQ explique que Steam Input peut aussi envoyer ce bouton au jeu et propose des boutons qui ouvrent la configuration de manette Steam du jeu ou les paramètres de manette de Steam. GameHQ ne lit ni ne modifie jamais les paramètres de Steam. Ces conseils peuvent être désactivés ou masqués pour chaque jeu.

## Limitations connues

- La capacité de la surcouche à isoler le jeu des entrées de la manette dépend du jeu et de la façon dont il lit la manette. Une DualSense filaire utilisant GameInput est bien testée ; ce n’est pas garanti pour XInput, Raw Input, le HID direct, Steam Input ou les manettes virtuelles.
- Avec Telegram Desktop et Discord, GameHQ ouvre l’application et vous terminez l’envoi depuis celle-ci ; GameHQ ne peut pas savoir si vous avez réellement envoyé la capture et n’indique donc jamais Envoyée pour ces applications. Avec Discord, vous collez vous-même la capture dans une discussion.
- Les envois vers les canaux Discord sont soumis à la limite de taille de fichier du serveur.
- Certains jeux se mettent en pause ou réagissent lorsqu’ils perdent le focus, par exemple lorsque Telegram ou Discord passe au premier plan après un partage. Windows ou une autre application de capture peut laisser visible la bordure jaune d’enregistrement.
