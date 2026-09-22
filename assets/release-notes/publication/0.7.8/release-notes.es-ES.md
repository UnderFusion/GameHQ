# GameHQ 0.7.8 (2026-09-22)

## Lo más destacado

- La superposición ahora funciona de forma mucho más fiable sobre juegos en modo sin bordes. Aparece por encima del juego, recibe la entrada del mando para navegar por la superposición en las rutas de GameInput compatibles y el juego sigue visible. Comprobado con un DualSense por cable en Indiana Jones and the Great Circle en modo sin bordes.
- Navegar por la superposición ya no controla el juego que hay debajo en las rutas de GameInput compatibles. Al cerrar la superposición, GameHQ espera a que sueltes los botones que tengas pulsados antes de devolver el mando al juego.
- Los clips de repetición son más seguros: guardar varias veces seguidas nunca sobrescribe un clip existente, y puedes guardar un clip justo después de que empiece la grabación.
- Nuevos preajustes de asignaciones: crea, edita y asigna distribuciones de mando para cada mando y cada juego.
- GameHQ recuerda dónde lo dejaste: la última página, la categoría de Configuración, el filtro de la galería y la categoría de la superposición de cada juego.

## Superposición

- La superposición puede tomar el foco del mando sin dejar de mostrarse sobre un juego visible en modo sin bordes y sin minimizarlo.
- En las rutas de GameInput compatibles, navegar por la superposición ya no controla también el juego.
- Al cerrar la superposición, se espera un momento a que los botones pulsados y los sticks vuelvan a reposo antes de devolver el mando al juego, para que una entrada mantenida no pase al juego.
- Abrir y cerrar la superposición es más rápido y fiable, y una pulsación repetida rápida ya no la cierra justo después de abrirse. Se respetan Alt-Tab y los demás cambios intencionados a otra aplicación.
- La superposición sigue al juego correcto cuando este vuelve a crear su ventana, la pierde o pierde el foco por un momento. Se cierra limpiamente cuando otra aplicación pasa a primer plano.
- La superposición se vuelve a abrir en la captura de pantalla o el clip que seleccionaste por última vez. Tras una captura nueva, empieza por el elemento más reciente.
- Los clips empiezan a reproducirse en la superposición sin que la vista previa desaparezca un instante.
- El fondo oscurecido ahora aparece a la vez que los menús de la superposición, en lugar de aparecer gradualmente después.
- Las indicaciones de controles de la parte inferior ahora tienen un pequeño fondo que sigue tu ajuste de oscurecimiento de la superposición, para que se lean bien sobre el juego.
- El juego al que estás jugando ahora se resalta correctamente al desplazarte por la barra lateral.
- La navegación con mando funciona mejor en los menús de la superposición, la galería y la reproducción de vídeo, y los botones de navegación se mantienen separados de los atajos de captura.

## Captura y repetición

- Al guardar varias veces seguidas, cada archivo recibe un nombre único en lugar de sobrescribir un clip existente. Una exportación fallida nunca elimina clips anteriores, y las miniaturas siempre corresponden al clip correcto.
- Al guardar una repetición se usa lo grabado hasta ese momento, aunque todavía no se haya completado la duración del búfer que configuraste.
- Una exportación de repetición conserva la grabación que necesita aunque cambies de juego o el búfer se reinicie, y GameHQ espera a que termine una exportación en curso antes de cerrarse.
- Las capturas de pantalla y los guardados de repeticiones se confirman al instante y después muestran claramente si se han guardado o han fallado. Las notificaciones se actualizan sin duplicarse y solo se muestra un número limitado a la vez.
- Las capturas fallidas u omitidas ahora explican el motivo, incluso con las notificaciones de éxito desactivadas.
- El estado de la repetición ahora indica si la grabación ha empezado realmente y si hay grabación utilizable, con mensajes claros cuando el búfer se está iniciando, está vacío o hay una exportación en curso.
- Las sesiones de repetición manuales ahora se mantienen tras cambiar la configuración y al hacer capturas de pantalla HDR al mismo tiempo. Una sesión manual que inicias pero no llegas a usar se desactiva pasado un tiempo.
- Guardar una repetición ya no detiene la captura mientras se crea la miniatura, y las capturas de pantalla hechas al mismo tiempo crean su carpeta de forma más fiable.
- Nueva opción para el borde amarillo de captura de Windows, con información más clara sobre permisos y compatibilidad del sistema. La grabación sigue funcionando cuando Windows no puede ocultar el borde.

## Mandos y entrada

- Detección de mandos y enrutamiento de la entrada más fiables cuando hay varios mandos o fuentes de entrada conectados.
- Al cambiar de fuente de entrada, reconectar mandos y seguir los botones mantenidos, ahora se evitan pulsaciones perdidas, pulsaciones duplicadas y entradas bloqueadas.
- Se han corregido las pulsaciones del botón PS que no se detectaban y las pulsaciones repetidas con retraso que podían volver a abrir la superposición justo después de cerrarla.
- Los gatillos, las pulsaciones de los sticks y los demás botones ahora se comportan igual en todas las fuentes de entrada compatibles, así que las asignaciones actúan de forma más predecible.
- Los mandos que usan GameInput siguen funcionando con normalidad, incluido el aislamiento de la superposición, cuando Windows no puede ofrecer la compatibilidad opcional con los botones Guide/Share.
- El mando que estás editando en Configuración sigue seleccionado cuando se activa otro mando.

## Preajustes de asignaciones

- Nueva biblioteca de preajustes para distribuciones de mando, donde puedes crear, renombrar, duplicar, editar y eliminar preajustes.
- Puedes asignar preajustes a mandos y juegos, con una opción de respaldo y selección automática para el juego que estés ejecutando.
- Tus asignaciones personalizadas se trasladan automáticamente al sistema de preajustes, y los datos originales se conservan como copia de seguridad.
- El cambio de preajuste tiene en cuenta los botones mantenidos y los gestos en curso, para que cambiar no active acciones por accidente.
- La interfaz distingue entre el preajuste asignado y el que estás editando, protege los cambios sin guardar y te permite crear una copia para un solo mando.
- Al eliminar un preajuste en uso, primero se te pide un sustituto o una opción de respaldo, y los cambios en preajustes compartidos se indican claramente.

## Interfaz, configuración y sonido

- GameHQ se vuelve a abrir en la última página, categoría de Configuración y filtro de galería que usaste. La superposición recuerda su última categoría por separado para cada juego.
- La ventana ahora se restaura correctamente en monitores situados a la izquierda o por encima de la pantalla principal. Una ventana que se abriría completamente fuera de la pantalla se devuelve a una pantalla conectada.
- La ventana principal y la superposición tienen cada una su propia escala, del 100 % al 200 %, que se conserva entre reinicios. Las ventanas pequeñas se distribuyen mejor.
- Los sonidos de captura son más fuertes y se distinguen mejor, y tienen su propio control de volumen y vista previa. El volumen de la interfaz y de la captura puede llegar al 300 %.
- Cambiar la configuración de notificaciones, sonido, borde de captura o sesiones manuales ya no descarta tu búfer de repetición.
- Al abrir la galería se mantiene su filtro guardado. Si el filtro guardado corresponde a un juego que ya no existe, se restablece de forma segura. Las categorías de Configuración se mantienen aunque cambie su orden.
- Se han corregido el contenido en blanco y los problemas de superposición de elementos al cambiar la escala de la interfaz, y los menús ahora aparecen por encima del contenido principal.
- Los controles de asignación muestran cuánto dura una pulsación mantenida y explican cómo funcionan los gestos de mantener pulsado.

## Actualizaciones, idiomas y diagnóstico

- Las notas de actualización aparecen en el idioma que has elegido, se muestran en inglés cuando no hay traducción disponible y se pueden leer sin conexión una vez cargadas.
- Las notas de actualización pueden incluir un enlace opcional a la versión en GitHub, al que puedes acceder con el mando.
- Se han ampliado las traducciones en todos los idiomas compatibles para los nuevos mensajes de captura, sonido, borde, pulsación mantenida, preajustes y foco.
- El diagnóstico copiado ahora incluye las asignaciones activas, la selección de preajuste, los cambios de fuente de entrada y el estado del foco de la superposición, con los identificadores de dispositivo anonimizados.
- El diagnóstico de captura sigue cada solicitud desde que pulsas el botón hasta que se guarda el archivo, lo que facilita localizar problemas. Los problemas de sonido se indican con una advertencia clara.

## Limitaciones conocidas

- Que la superposición aísle el mando del juego depende del juego y de cómo lee el mando. Un DualSense por cable con GameInput está bien probado. No está garantizado con XInput, Raw Input, HID directo, Steam Input ni mandos virtuales.
- El cambio de mando con DSX en ejecución solo está verificado en parte. Tú mantienes el control de tu configuración de DSX; GameHQ no instala ni gestiona controladores de mandos virtuales ni de ocultación de dispositivos.
- Algunos juegos se pausan o reaccionan al perder el foco. Windows u otra aplicación de captura pueden mantener visible el borde amarillo de grabación.
- Si no hay una nota de la versión 0.7.8 traducida, GameHQ la muestra en inglés.
